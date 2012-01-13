/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011	ProFUSION embedded systems
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/sockios.h>
#include <string.h>
#include <fcntl.h>
#include <linux/if_tun.h>

#include "connman.h"

#include <gdhcp/gdhcp.h>

#include <gdbus.h>

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define BRIDGE_PROC_DIR "/proc/sys/net/bridge"

#define BRIDGE_NAME "tether"
#define BRIDGE_IP "192.168.218.1"
#define BRIDGE_BCAST "192.168.218.255"
#define BRIDGE_SUBNET "255.255.255.0"
#define BRIDGE_IP_START "192.168.218.100"
#define BRIDGE_IP_END "192.168.218.200"
#define BRIDGE_DNS "8.8.8.8"

#define DEFAULT_MTU	1500

#define PRIVATE_NETWORK_IP "192.168.219.1"
#define PRIVATE_NETWORK_PEER_IP "192.168.219.2"
#define PRIVATE_NETWORK_NETMASK "255.255.255.0"
#define PRIVATE_NETWORK_PRIMARY_DNS BRIDGE_DNS
#define PRIVATE_NETWORK_SECONDARY_DNS "8.8.4.4"

static char *default_interface = NULL;
static volatile int tethering_enabled;
static GDHCPServer *tethering_dhcp_server = NULL;
static DBusConnection *connection;
static GHashTable *pn_hash;

struct connman_private_network {
	char *owner;
	char *path;
	guint watch;
	DBusMessage *msg;
	DBusMessage *reply;
	int fd;
	char *interface;
	int index;
	guint iface_watch;
	const char *server_ip;
	const char *peer_ip;
	const char *primary_dns;
	const char *secondary_dns;
};

const char *__connman_tethering_get_bridge(void)
{
	struct stat st;

	if (stat(BRIDGE_PROC_DIR, &st) < 0) {
		connman_error("Missing support for 802.1d ethernet bridging");
		return NULL;
	}

	return BRIDGE_NAME;
}

static void dhcp_server_debug(const char *str, void *data)
{
	connman_info("%s: %s\n", (const char *) data, str);
}

static void dhcp_server_error(GDHCPServerError error)
{
	switch (error) {
	case G_DHCP_SERVER_ERROR_NONE:
		connman_error("OK");
		break;
	case G_DHCP_SERVER_ERROR_INTERFACE_UNAVAILABLE:
		connman_error("Interface unavailable");
		break;
	case G_DHCP_SERVER_ERROR_INTERFACE_IN_USE:
		connman_error("Interface in use");
		break;
	case G_DHCP_SERVER_ERROR_INTERFACE_DOWN:
		connman_error("Interface down");
		break;
	case G_DHCP_SERVER_ERROR_NOMEM:
		connman_error("No memory");
		break;
	case G_DHCP_SERVER_ERROR_INVALID_INDEX:
		connman_error("Invalid index");
		break;
	case G_DHCP_SERVER_ERROR_INVALID_OPTION:
		connman_error("Invalid option");
		break;
	case G_DHCP_SERVER_ERROR_IP_ADDRESS_INVALID:
		connman_error("Invalid address");
		break;
	}
}

static GDHCPServer *dhcp_server_start(const char *bridge,
				const char *router, const char* subnet,
				const char *start_ip, const char *end_ip,
				unsigned int lease_time, const char *dns)
{
	GDHCPServerError error;
	GDHCPServer *dhcp_server;
	int index;

	DBG("");

	index = connman_inet_ifindex(bridge);
	if (index < 0)
		return NULL;

	dhcp_server = g_dhcp_server_new(G_DHCP_IPV4, index, &error);
	if (dhcp_server == NULL) {
		dhcp_server_error(error);
		return NULL;
	}

	g_dhcp_server_set_debug(dhcp_server, dhcp_server_debug, "DHCP server");

	g_dhcp_server_set_lease_time(dhcp_server, lease_time);
	g_dhcp_server_set_option(dhcp_server, G_DHCP_SUBNET, subnet);
	g_dhcp_server_set_option(dhcp_server, G_DHCP_ROUTER, router);
	g_dhcp_server_set_option(dhcp_server, G_DHCP_DNS_SERVER, dns);
	g_dhcp_server_set_ip_range(dhcp_server, start_ip, end_ip);

	g_dhcp_server_start(dhcp_server);

	return dhcp_server;
}

static void dhcp_server_stop(GDHCPServer *server)
{
	if (server == NULL)
		return;

	g_dhcp_server_unref(server);
}

static int set_forward_delay(const char *name, unsigned int delay)
{
	FILE *f;
	char *forward_delay_path;

	forward_delay_path =
		g_strdup_printf("/sys/class/net/%s/bridge/forward_delay", name);

	if (forward_delay_path == NULL)
		return -ENOMEM;

	f = fopen(forward_delay_path, "r+");

	g_free(forward_delay_path);

	if (f == NULL)
		return -errno;

	fprintf(f, "%d", delay);

	fclose(f);

	return 0;
}

static int create_bridge(const char *name)
{
	int sk, err;

	DBG("name %s", name);

	sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return -EOPNOTSUPP;

	if (ioctl(sk, SIOCBRADDBR, name) == -1) {
		err = -errno;
		if (err != -EEXIST)
			return -EOPNOTSUPP;
	}

	err = set_forward_delay(name, 0);

	if (err < 0)
		ioctl(sk, SIOCBRDELBR, name);

	close(sk);

	return err;
}

static int remove_bridge(const char *name)
{
	int sk, err;

	DBG("name %s", name);

	sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return -EOPNOTSUPP;

	err = ioctl(sk, SIOCBRDELBR, name);

	close(sk);

	if (err < 0)
		return -EOPNOTSUPP;

	return 0;
}

static int enable_bridge(const char *name)
{
	int err, index;

	index = connman_inet_ifindex(name);
	if (index < 0)
		return index;

	err = __connman_inet_modify_address(RTM_NEWADDR,
			NLM_F_REPLACE | NLM_F_ACK, index, AF_INET,
					BRIDGE_IP, NULL, 24, BRIDGE_BCAST);
	if (err < 0)
		return err;

	return connman_inet_ifup(index);
}

static int disable_bridge(const char *name)
{
	int index;

	index = connman_inet_ifindex(name);
	if (index < 0)
		return index;

	return connman_inet_ifdown(index);
}

static int enable_ip_forward(connman_bool_t enable)
{

	FILE *f;

	f = fopen("/proc/sys/net/ipv4/ip_forward", "r+");
	if (f == NULL)
		return -errno;

	if (enable == TRUE)
		fprintf(f, "1");
	else
		fprintf(f, "0");

	fclose(f);

	return 0;
}

static int enable_nat(const char *interface)
{
	int err;

	if (interface == NULL)
		return 0;

	/* Enable IPv4 forwarding */
	err = enable_ip_forward(TRUE);
	if (err < 0)
		return err;

	/* POSTROUTING flush */
	err = __connman_iptables_command("-t nat -F POSTROUTING");
	if (err < 0)
		return err;

	/* Enable masquerading */
	err = __connman_iptables_command("-t nat -A POSTROUTING "
					"-o %s -j MASQUERADE", interface);
	if (err < 0)
		return err;

	return __connman_iptables_commit("nat");
}

static void disable_nat(const char *interface)
{
	int err;

	/* Disable IPv4 forwarding */
	enable_ip_forward(FALSE);

	/* POSTROUTING flush */
	err = __connman_iptables_command("-t nat -F POSTROUTING");
	if (err < 0)
		return;

	__connman_iptables_commit("nat");
}

void __connman_tethering_set_enabled(void)
{
	int err;
	const char *dns;

	DBG("enabled %d", tethering_enabled + 1);

	if (__sync_fetch_and_add(&tethering_enabled, 1) != 0)
		return;

	err = create_bridge(BRIDGE_NAME);
	if (err < 0)
		return;

	err = enable_bridge(BRIDGE_NAME);
	if (err < 0 && err != -EALREADY) {
		remove_bridge(BRIDGE_NAME);
		return;
	}

	dns = BRIDGE_IP;
	if (__connman_dnsproxy_add_listener(BRIDGE_NAME) < 0) {
		connman_error("Can't add listener %s to DNS proxy",
								BRIDGE_NAME);
		dns = BRIDGE_DNS;
	}

	tethering_dhcp_server =
		dhcp_server_start(BRIDGE_NAME,
					BRIDGE_IP, BRIDGE_SUBNET,
					BRIDGE_IP_START, BRIDGE_IP_END,
					24 * 3600, dns);
	if (tethering_dhcp_server == NULL) {
		disable_bridge(BRIDGE_NAME);
		remove_bridge(BRIDGE_NAME);
		return;
	}

	enable_nat(default_interface);

	DBG("tethering started");
}

void __connman_tethering_set_disabled(void)
{
	DBG("enabled %d", tethering_enabled - 1);

	__connman_dnsproxy_remove_listener(BRIDGE_NAME);

	if (__sync_fetch_and_sub(&tethering_enabled, 1) != 1)
		return;

	disable_nat(default_interface);

	dhcp_server_stop(tethering_dhcp_server);

	tethering_dhcp_server = NULL;

	disable_bridge(BRIDGE_NAME);

	remove_bridge(BRIDGE_NAME);

	DBG("tethering stopped");
}

void __connman_tethering_update_interface(const char *interface)
{
	DBG("interface %s", interface);

	g_free(default_interface);

	if (interface == NULL) {
		disable_nat(interface);
		default_interface = NULL;

		return;
	}

	default_interface = g_strdup(interface);

	__sync_synchronize();
	if (tethering_enabled == 0)
		return;

	enable_nat(interface);
}

static void setup_tun_interface(unsigned int flags, unsigned change,
		void *data)
{
	struct connman_private_network *pn = data;
	unsigned char prefixlen;
	DBusMessageIter array, dict;
	int err;

	DBG("index %d flags %d change %d", pn->index,  flags, change);

	if (flags & IFF_UP)
		return;

	prefixlen =
		__connman_ipconfig_netmask_prefix_len(PRIVATE_NETWORK_NETMASK);

	if ((__connman_inet_modify_address(RTM_NEWADDR,
				NLM_F_REPLACE | NLM_F_ACK, pn->index, AF_INET,
				pn->server_ip, pn->peer_ip,
				prefixlen, NULL)) < 0) {
		DBG("address setting failed");
		return;
	}

	connman_inet_ifup(pn->index);

	err = enable_nat(default_interface);
	if (err < 0) {
		connman_error("failed to enable NAT on %s", default_interface);
		goto error;
	}

	dbus_message_iter_init_append(pn->reply, &array);

	dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH,
						&pn->path);

	connman_dbus_dict_open(&array, &dict);

	connman_dbus_dict_append_basic(&dict, "ServerIPv4",
					DBUS_TYPE_STRING, &pn->server_ip);
	connman_dbus_dict_append_basic(&dict, "PeerIPv4",
					DBUS_TYPE_STRING, &pn->peer_ip);
	connman_dbus_dict_append_basic(&dict, "PrimaryDNS",
					DBUS_TYPE_STRING, &pn->primary_dns);
	connman_dbus_dict_append_basic(&dict, "SecondaryDNS",
					DBUS_TYPE_STRING, &pn->secondary_dns);

	connman_dbus_dict_close(&array, &dict);

	dbus_message_iter_append_basic(&array, DBUS_TYPE_UNIX_FD, &pn->fd);

	g_dbus_send_message(connection, pn->reply);

	return;

error:
	pn->reply = __connman_error_failed(pn->msg, -err);
	g_dbus_send_message(connection, pn->reply);

	g_hash_table_remove(pn_hash, pn->path);
}

static void remove_private_network(gpointer user_data)
{
	struct connman_private_network *pn = user_data;

	disable_nat(default_interface);
	connman_rtnl_remove_watch(pn->iface_watch);

	if (pn->watch > 0) {
		g_dbus_remove_watch(connection, pn->watch);
		pn->watch = 0;
	}

	close(pn->fd);

	g_free(pn->interface);
	g_free(pn->owner);
	g_free(pn->path);
	g_free(pn);
}

static void owner_disconnect(DBusConnection *connection, void *user_data)
{
	struct connman_private_network *pn = user_data;

	DBG("%s died", pn->owner);

	pn->watch = 0;

	g_hash_table_remove(pn_hash, pn->path);
}

int __connman_private_network_request(DBusMessage *msg, const char *owner)
{
	struct connman_private_network *pn;
	char *iface = NULL;
	char *path = NULL;
	int index, fd, err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EINVAL;

	fd = connman_inet_create_tunnel(&iface);
	if (fd < 0)
		return fd;

	path = g_strdup_printf("/tethering/%s", iface);

	pn = g_hash_table_lookup(pn_hash, path);
	if (pn) {
		g_free(path);
		g_free(iface);
		close(fd);
		return -EEXIST;
	}

	index = connman_inet_ifindex(iface);
	if (index < 0) {
		err = -ENODEV;
		goto error;
	}
	DBG("interface %s", iface);

	err = connman_inet_set_mtu(index, DEFAULT_MTU);

	pn = g_try_new0(struct connman_private_network, 1);
	if (pn == NULL) {
		err = -ENOMEM;
		goto error;
	}

	pn->owner = g_strdup(owner);
	pn->path = path;
	pn->watch = g_dbus_add_disconnect_watch(connection, pn->owner,
					owner_disconnect, pn, NULL);
	pn->msg = msg;
	pn->reply = dbus_message_new_method_return(pn->msg);
	if (pn->reply == NULL)
		goto error;

	pn->fd = fd;
	pn->interface = iface;
	pn->index = index;
	pn->server_ip = PRIVATE_NETWORK_IP;
	pn->peer_ip = PRIVATE_NETWORK_PEER_IP;
	pn->primary_dns = PRIVATE_NETWORK_PRIMARY_DNS;
	pn->secondary_dns = PRIVATE_NETWORK_SECONDARY_DNS;

	pn->iface_watch = connman_rtnl_add_newlink_watch(index,
						setup_tun_interface, pn);

	g_hash_table_insert(pn_hash, pn->path, pn);

	return 0;

error:
	close(fd);
	g_free(iface);
	g_free(path);
	g_free(pn);
	return err;
}

int __connman_private_network_release(const char *path)
{
	struct connman_private_network *pn;

	pn = g_hash_table_lookup(pn_hash, path);
	if (pn == NULL)
		return -EACCES;

	g_hash_table_remove(pn_hash, path);
	return 0;
}

int __connman_tethering_init(void)
{
	DBG("");

	tethering_enabled = 0;

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -EFAULT;

	pn_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, remove_private_network);

	return 0;
}

void __connman_tethering_cleanup(void)
{
	DBG("");

	__sync_synchronize();
	if (tethering_enabled == 0) {
		if (tethering_dhcp_server)
			dhcp_server_stop(tethering_dhcp_server);
		disable_bridge(BRIDGE_NAME);
		remove_bridge(BRIDGE_NAME);
	}

	if (connection == NULL)
		return;

	g_hash_table_destroy(pn_hash);
	dbus_connection_unref(connection);
}
