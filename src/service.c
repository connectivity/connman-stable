/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2010  Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <gdbus.h>

#include <connman/storage.h>

#include "connman.h"

#define CONNECT_TIMEOUT		120

static DBusConnection *connection = NULL;

static GSequence *service_list = NULL;
static GHashTable *service_hash = NULL;
static GSList *counter_list = NULL;

struct connman_stats {
	connman_bool_t valid;
	connman_bool_t enabled;
	struct connman_stats_data data_last;
	struct connman_stats_data data;
	GTimer *timer;
};

struct connman_stats_counter {
	connman_bool_t append_all;
	struct connman_stats stats;
	struct connman_stats stats_roaming;
};

struct connman_service {
	int refcount;
	int session_usage_count;
	char *identifier;
	char *path;
	enum connman_service_type type;
	enum connman_service_security security;
	enum connman_service_state state;
	enum connman_service_state state_ipv4;
	enum connman_service_state state_ipv6;
	enum connman_service_error error;
	connman_uint8_t strength;
	connman_bool_t favorite;
	connman_bool_t immutable;
	connman_bool_t hidden;
	connman_bool_t ignore;
	connman_bool_t autoconnect;
	connman_bool_t userconnect;
	GTimeVal modified;
	unsigned int order;
	char *name;
	char *passphrase;
	char *agent_passphrase;
	connman_bool_t roaming;
	connman_bool_t login_required;
	connman_bool_t network_created;
	struct connman_ipconfig *ipconfig_ipv4;
	struct connman_ipconfig *ipconfig_ipv6;
	struct connman_network *network;
	struct connman_provider *provider;
	char **nameservers;
	char **nameservers_config;
	char **nameservers_auto;
	char **domains;
	char *domainname;
	char **timeservers;
	/* 802.1x settings from the config files */
	char *eap;
	char *identity;
	char *agent_identity;
	char *ca_cert_file;
	char *client_cert_file;
	char *private_key_file;
	char *private_key_passphrase;
	char *phase2;
	DBusMessage *pending;
	guint timeout;
	struct connman_stats stats;
	struct connman_stats stats_roaming;
	GHashTable *counter_table;
	enum connman_service_proxy_method proxy;
	enum connman_service_proxy_method proxy_config;
	char **proxies;
	char **excludes;
	char *pac;
	connman_bool_t wps;
};

static void append_path(gpointer value, gpointer user_data)
{
	struct connman_service *service = value;
	DBusMessageIter *iter = user_data;

	if (service->path == NULL || service->hidden == TRUE)
		return;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH,
							&service->path);
}

void __connman_service_list(DBusMessageIter *iter, void *user_data)
{
	if (service_list == NULL)
		return;

	g_sequence_foreach(service_list, append_path, iter);
}

struct find_data {
	const char *path;
	struct connman_service *service;
};

static void compare_path(gpointer value, gpointer user_data)
{
	struct connman_service *service = value;
	struct find_data *data = user_data;

	if (data->service != NULL)
		return;

	if (g_strcmp0(service->path, data->path) == 0)
		data->service = service;
}

static struct connman_service *find_service(const char *path)
{
	struct find_data data = { .path = path, .service = NULL };

	DBG("path %s", path);

	g_sequence_foreach(service_list, compare_path, &data);

	return data.service;
}

const char *__connman_service_type2string(enum connman_service_type type)
{
	switch (type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
		break;
	case CONNMAN_SERVICE_TYPE_SYSTEM:
		return "system";
	case CONNMAN_SERVICE_TYPE_ETHERNET:
		return "ethernet";
	case CONNMAN_SERVICE_TYPE_WIFI:
		return "wifi";
	case CONNMAN_SERVICE_TYPE_WIMAX:
		return "wimax";
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
		return "bluetooth";
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		return "cellular";
	case CONNMAN_SERVICE_TYPE_GPS:
		return "gps";
	case CONNMAN_SERVICE_TYPE_VPN:
		return "vpn";
	case CONNMAN_SERVICE_TYPE_GADGET:
		return "gadget";
	}

	return NULL;
}

static const char *security2string(enum connman_service_security security)
{
	switch (security) {
	case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		break;
	case CONNMAN_SERVICE_SECURITY_NONE:
		return "none";
	case CONNMAN_SERVICE_SECURITY_WEP:
		return "wep";
	case CONNMAN_SERVICE_SECURITY_PSK:
		return "psk";
	case CONNMAN_SERVICE_SECURITY_8021X:
		return "ieee8021x";
	case CONNMAN_SERVICE_SECURITY_WPA:
		return "wpa";
	case CONNMAN_SERVICE_SECURITY_RSN:
		return "rsn";
	}

	return NULL;
}

static const char *state2string(enum connman_service_state state)
{
	switch (state) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
		break;
	case CONNMAN_SERVICE_STATE_IDLE:
		return "idle";
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
		return "association";
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
		return "configuration";
	case CONNMAN_SERVICE_STATE_READY:
		return "ready";
	case CONNMAN_SERVICE_STATE_ONLINE:
		return "online";
	case CONNMAN_SERVICE_STATE_DISCONNECT:
		return "disconnect";
	case CONNMAN_SERVICE_STATE_FAILURE:
		return "failure";
	}

	return NULL;
}

static const char *error2string(enum connman_service_error error)
{
	switch (error) {
	case CONNMAN_SERVICE_ERROR_UNKNOWN:
		break;
	case CONNMAN_SERVICE_ERROR_OUT_OF_RANGE:
		return "out-of-range";
	case CONNMAN_SERVICE_ERROR_PIN_MISSING:
		return "pin-missing";
	case CONNMAN_SERVICE_ERROR_DHCP_FAILED:
		return "dhcp-failed";
	case CONNMAN_SERVICE_ERROR_CONNECT_FAILED:
		return "connect-failed";
	case CONNMAN_SERVICE_ERROR_LOGIN_FAILED:
		return "login-failed";
	case CONNMAN_SERVICE_ERROR_AUTH_FAILED:
		return "auth-failed";
	case CONNMAN_SERVICE_ERROR_INVALID_KEY:
		return "invalid-key";
	}

	return NULL;
}

static enum connman_service_error string2error(const char *error)
{
	if (g_strcmp0(error, "dhcp-failed") == 0)
		return CONNMAN_SERVICE_ERROR_DHCP_FAILED;
	else if (g_strcmp0(error, "pin-missing") == 0)
		return CONNMAN_SERVICE_ERROR_PIN_MISSING;
	else if (g_strcmp0(error, "invalid-key") == 0)
		return CONNMAN_SERVICE_ERROR_INVALID_KEY;

	return CONNMAN_SERVICE_ERROR_UNKNOWN;
}

static const char *proxymethod2string(enum connman_service_proxy_method method)
{
	switch (method) {
	case CONNMAN_SERVICE_PROXY_METHOD_DIRECT:
		return "direct";
	case CONNMAN_SERVICE_PROXY_METHOD_MANUAL:
		return "manual";
	case CONNMAN_SERVICE_PROXY_METHOD_AUTO:
		return "auto";
	case CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN:
		break;
	}

	return NULL;
}

static enum connman_service_proxy_method string2proxymethod(const char *method)
{
	if (g_strcmp0(method, "direct") == 0)
		return CONNMAN_SERVICE_PROXY_METHOD_DIRECT;
	else if (g_strcmp0(method, "auto") == 0)
		return CONNMAN_SERVICE_PROXY_METHOD_AUTO;
	else if (g_strcmp0(method, "manual") == 0)
		return CONNMAN_SERVICE_PROXY_METHOD_MANUAL;
	else
		return CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN;
}

static int service_load(struct connman_service *service)
{
	GKeyFile *keyfile;
	GError *error = NULL;
	gsize length;
	gchar *str;
	connman_bool_t autoconnect;
	unsigned int ssid_len;
	int err = 0;

	DBG("service %p", service);

	keyfile = connman_storage_load_service(service->identifier);
	if (keyfile == NULL)
		return -EIO;

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		break;
	case CONNMAN_SERVICE_TYPE_WIFI:
		if (service->name == NULL) {
			gchar *name;

			name = g_key_file_get_string(keyfile,
					service->identifier, "Name", NULL);
			if (name != NULL) {
				g_free(service->name);
				service->name = name;
			}

			if (service->network != NULL)
				connman_network_set_name(service->network,
									name);
		}

		if (service->network &&
				connman_network_get_blob(service->network,
					"WiFi.SSID", &ssid_len) == NULL) {
			gchar *hex_ssid;

			hex_ssid = g_key_file_get_string(keyfile,
							service->identifier,
								"SSID", NULL);

			if (hex_ssid != NULL) {
				gchar *ssid;
				unsigned int i, j = 0, hex;
				size_t hex_ssid_len = strlen(hex_ssid);

				ssid = g_try_malloc0(hex_ssid_len / 2);
				if (ssid == NULL) {
					g_free(hex_ssid);
					err = -ENOMEM;
					goto done;
				}

				for (i = 0; i < hex_ssid_len; i += 2) {
					sscanf(hex_ssid + i, "%02x", &hex);
					ssid[j++] = hex;
				}

				connman_network_set_blob(service->network,
					"WiFi.SSID", ssid, hex_ssid_len / 2);
			}

			g_free(hex_ssid);
		}
		/* fall through */

	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		service->favorite = g_key_file_get_boolean(keyfile,
				service->identifier, "Favorite", NULL);

		autoconnect = g_key_file_get_boolean(keyfile,
				service->identifier, "AutoConnect", &error);
		if (error == NULL)
			service->autoconnect = autoconnect;
		g_clear_error(&error);

		str = g_key_file_get_string(keyfile,
				service->identifier, "Failure", NULL);
		if (str != NULL) {
			if (service->favorite == FALSE)
				service->state_ipv4 = service->state_ipv6 =
					CONNMAN_SERVICE_STATE_FAILURE;
			service->error = string2error(str);
			g_free(str);
		}
		break;
	}

	str = g_key_file_get_string(keyfile,
				service->identifier, "Modified", NULL);
	if (str != NULL) {
		g_time_val_from_iso8601(str, &service->modified);
		g_free(str);
	}

	str = g_key_file_get_string(keyfile,
				service->identifier, "Passphrase", NULL);
	if (str != NULL) {
		g_free(service->passphrase);
		service->passphrase = str;
	}

	if (service->ipconfig_ipv4 != NULL)
		__connman_ipconfig_load(service->ipconfig_ipv4, keyfile,
					service->identifier, "IPv4.");

	if (service->ipconfig_ipv6 != NULL)
		__connman_ipconfig_load(service->ipconfig_ipv6, keyfile,
					service->identifier, "IPv6.");

	service->nameservers_config = g_key_file_get_string_list(keyfile,
			service->identifier, "Nameservers", &length, NULL);
	if (service->nameservers_config != NULL && length == 0) {
		g_strfreev(service->nameservers_config);
		service->nameservers_config = NULL;
	}

	service->domains = g_key_file_get_string_list(keyfile,
			service->identifier, "Domains", &length, NULL);
	if (service->domains != NULL && length == 0) {
		g_strfreev(service->domains);
		service->domains = NULL;
	}

	str = g_key_file_get_string(keyfile,
				service->identifier, "Proxy.Method", NULL);
	if (str != NULL)
		service->proxy_config = string2proxymethod(str);

	g_free(str);

	service->proxies = g_key_file_get_string_list(keyfile,
			service->identifier, "Proxy.Servers", &length, NULL);
	if (service->proxies != NULL && length == 0) {
		g_strfreev(service->proxies);
		service->proxies = NULL;
	}

	service->excludes = g_key_file_get_string_list(keyfile,
			service->identifier, "Proxy.Excludes", &length, NULL);
	if (service->excludes != NULL && length == 0) {
		g_strfreev(service->excludes);
		service->excludes = NULL;
	}

	str = g_key_file_get_string(keyfile,
				service->identifier, "Proxy.URL", NULL);
	if (str != NULL) {
		g_free(service->pac);
		service->pac = str;
	}

done:
	g_key_file_free(keyfile);

	return err;
}

static int service_save(struct connman_service *service)
{
	GKeyFile *keyfile;
	gchar *str;
	guint freq;
	const char *cst_str = NULL;
	int err = 0;

	DBG("service %p", service);

	keyfile = __connman_storage_open_service(service->identifier);
	if (keyfile == NULL)
		return -EIO;

	if (service->name != NULL)
		g_key_file_set_string(keyfile, service->identifier,
						"Name", service->name);

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		break;
	case CONNMAN_SERVICE_TYPE_WIFI:
		if (service->network) {
			const unsigned char *ssid;
			unsigned int ssid_len = 0;

			ssid = connman_network_get_blob(service->network,
							"WiFi.SSID", &ssid_len);

			if (ssid != NULL && ssid_len > 0 && ssid[0] != '\0') {
				char *identifier = service->identifier;
				GString *str;
				unsigned int i;

				str = g_string_sized_new(ssid_len * 2);
				if (str == NULL) {
					err = -ENOMEM;
					goto done;
				}

				for (i = 0; i < ssid_len; i++)
					g_string_append_printf(str,
							"%02x", ssid[i]);

				g_key_file_set_string(keyfile, identifier,
							"SSID", str->str);

				g_string_free(str, TRUE);
			}

			freq = connman_network_get_frequency(service->network);
			g_key_file_set_integer(keyfile, service->identifier,
						"Frequency", freq);
		}
		/* fall through */

	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		g_key_file_set_boolean(keyfile, service->identifier,
					"Favorite", service->favorite);

		if (service->favorite == TRUE)
			g_key_file_set_boolean(keyfile, service->identifier,
					"AutoConnect", service->autoconnect);

		if (service->state_ipv4 == CONNMAN_SERVICE_STATE_FAILURE ||
			service->state_ipv6 == CONNMAN_SERVICE_STATE_FAILURE) {
			const char *failure = error2string(service->error);
			if (failure != NULL)
				g_key_file_set_string(keyfile,
							service->identifier,
							"Failure", failure);
		} else {
			g_key_file_remove_key(keyfile, service->identifier,
							"Failure", NULL);
		}
		break;
	}

	str = g_time_val_to_iso8601(&service->modified);
	if (str != NULL) {
		g_key_file_set_string(keyfile, service->identifier,
							"Modified", str);
		g_free(str);
	}

	if (service->passphrase != NULL && strlen(service->passphrase) > 0)
		g_key_file_set_string(keyfile, service->identifier,
					"Passphrase", service->passphrase);
	else
		g_key_file_remove_key(keyfile, service->identifier,
							"Passphrase", NULL);

	if (service->ipconfig_ipv4 != NULL)
		__connman_ipconfig_save(service->ipconfig_ipv4, keyfile,
					service->identifier, "IPv4.");

	if (service->ipconfig_ipv6 != NULL)
		__connman_ipconfig_save(service->ipconfig_ipv6, keyfile,
						service->identifier, "IPv6.");

	if (service->nameservers_config != NULL) {
		guint len = g_strv_length(service->nameservers_config);

		g_key_file_set_string_list(keyfile, service->identifier,
								"Nameservers",
				(const gchar **) service->nameservers_config, len);
	} else
	g_key_file_remove_key(keyfile, service->identifier,
							"Nameservers", NULL);

	if (service->domains != NULL) {
		guint len = g_strv_length(service->domains);

		g_key_file_set_string_list(keyfile, service->identifier,
								"Domains",
				(const gchar **) service->domains, len);
	} else
		g_key_file_remove_key(keyfile, service->identifier,
							"Domains", NULL);

	cst_str = proxymethod2string(service->proxy_config);
	if (cst_str != NULL)
		g_key_file_set_string(keyfile, service->identifier,
				"Proxy.Method", cst_str);

	if (service->proxies != NULL) {
		guint len = g_strv_length(service->proxies);

		g_key_file_set_string_list(keyfile, service->identifier,
				"Proxy.Servers",
				(const gchar **) service->proxies, len);
	} else
		g_key_file_remove_key(keyfile, service->identifier,
						"Proxy.Servers", NULL);

	if (service->excludes != NULL) {
		guint len = g_strv_length(service->excludes);

		g_key_file_set_string_list(keyfile, service->identifier,
				"Proxy.Excludes",
				(const gchar **) service->excludes, len);
	} else
		g_key_file_remove_key(keyfile, service->identifier,
						"Proxy.Excludes", NULL);

	if (service->pac != NULL && strlen(service->pac) > 0)
		g_key_file_set_string(keyfile, service->identifier,
					"Proxy.URL", service->pac);
	else
		g_key_file_remove_key(keyfile, service->identifier,
							"Proxy.URL", NULL);

done:
	__connman_storage_save_service(keyfile, service->identifier);

	g_key_file_free(keyfile);

	return err;
}

static guint changed_timeout = 0;

static gboolean notify_services_changed(gpointer user_data)
{
	changed_timeout = 0;

	connman_dbus_property_changed_array(CONNMAN_MANAGER_PATH,
				CONNMAN_MANAGER_INTERFACE, "Services",
				DBUS_TYPE_OBJECT_PATH, __connman_service_list,
				NULL);

	return FALSE;
}

static void services_changed(gboolean delayed)
{
	DBG("");

	if (changed_timeout > 0) {
		g_source_remove(changed_timeout);
		changed_timeout = 0;
	}

	if (__connman_connection_update_gateway() == TRUE) {
		notify_services_changed(NULL);
		return;
	}

	if (delayed == FALSE) {
		notify_services_changed(NULL);
		return;
	}

	changed_timeout = g_timeout_add_seconds(1, notify_services_changed,
								 NULL);
}

static enum connman_service_state combine_state(
					enum connman_service_state state_a,
					enum connman_service_state state_b)
{
	enum connman_service_state result;

	if (state_a == state_b) {
		result = state_a;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_UNKNOWN) {
		result = state_b;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_UNKNOWN) {
		result = state_a;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_IDLE) {
		result = state_b;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_IDLE) {
		result = state_a;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_ONLINE) {
		result = state_a;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_ONLINE) {
		result = state_b;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_READY) {
		result = state_a;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_READY) {
		result = state_b;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_CONFIGURATION) {
		result = state_a;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_CONFIGURATION) {
		result = state_b;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_ASSOCIATION) {
		result = state_a;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_ASSOCIATION) {
		result = state_b;
		goto done;
	}

	if (state_a == CONNMAN_SERVICE_STATE_DISCONNECT) {
		result = state_a;
		goto done;
	}

	if (state_b == CONNMAN_SERVICE_STATE_DISCONNECT) {
		result = state_b;
		goto done;
	}

	result = CONNMAN_SERVICE_STATE_FAILURE;

done:
	return result;
}

static connman_bool_t is_connecting_state(struct connman_service *service,
					enum connman_service_state state)
{
	switch (state) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
	case CONNMAN_SERVICE_STATE_IDLE:
	case CONNMAN_SERVICE_STATE_FAILURE:
		if (service->network != NULL)
			return connman_network_get_connecting(service->network);
	case CONNMAN_SERVICE_STATE_DISCONNECT:
	case CONNMAN_SERVICE_STATE_READY:
	case CONNMAN_SERVICE_STATE_ONLINE:
		break;
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
		return TRUE;
	}

	return FALSE;
}

static connman_bool_t is_connected_state(const struct connman_service *service,
					enum connman_service_state state)
{
	switch (state) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
	case CONNMAN_SERVICE_STATE_IDLE:
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
	case CONNMAN_SERVICE_STATE_DISCONNECT:
	case CONNMAN_SERVICE_STATE_FAILURE:
		break;
	case CONNMAN_SERVICE_STATE_READY:
	case CONNMAN_SERVICE_STATE_ONLINE:
		return TRUE;
	}

	return FALSE;
}

static connman_bool_t is_connecting(struct connman_service *service)
{
	return is_connecting_state(service, service->state);
}

static connman_bool_t is_connected(struct connman_service *service)
{
	return is_connected_state(service, service->state);
}

static void update_nameservers(struct connman_service *service)
{
	const char *ifname;

	if (service->ipconfig_ipv4)
		ifname = connman_ipconfig_get_ifname(service->ipconfig_ipv4);
	else if (service->ipconfig_ipv6)
		ifname = connman_ipconfig_get_ifname(service->ipconfig_ipv6);
	else
		ifname = NULL;

	if (ifname == NULL)
		return;

	switch (combine_state(service->state_ipv4, service->state_ipv6)) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
	case CONNMAN_SERVICE_STATE_IDLE:
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
		return;
	case CONNMAN_SERVICE_STATE_FAILURE:
	case CONNMAN_SERVICE_STATE_DISCONNECT:
		connman_resolver_remove_all(ifname);
		return;
	case CONNMAN_SERVICE_STATE_READY:
	case CONNMAN_SERVICE_STATE_ONLINE:
		break;
	}

	if (service->nameservers_config != NULL) {
		int i;

		for (i = 0; service->nameservers_config[i] != NULL; i++) {
			connman_resolver_append(ifname, NULL,
						service->nameservers_config[i]);
		}
	} else if (service->nameservers != NULL) {
		int i;

		for (i = 0; service->nameservers[i] != NULL; i++) {
			connman_resolver_append(ifname, NULL,
						service->nameservers[i]);
		}
	}

	if (service->domains != NULL) {
		int i;

		for (i = 0; service->domains[i]; i++)
			connman_resolver_append(ifname,	service->domains[i],
						NULL);
	} else if (service->domainname != NULL)
		connman_resolver_append(ifname, service->domainname, NULL);

	connman_resolver_flush();
}

/*
 * The is_auto variable is set to true when IPv6 autoconf nameservers are
 * inserted to resolver via netlink message (see rtnl.c:rtnl_newnduseropt()
 * for details) and not through service.c
 */
int __connman_service_nameserver_append(struct connman_service *service,
				const char *nameserver, gboolean is_auto)
{
	char **nameservers;
	int len, i;

	DBG("service %p nameserver %s auto %d",	service, nameserver, is_auto);

	if (nameserver == NULL)
		return -EINVAL;

	if (is_auto == TRUE)
		nameservers = service->nameservers_auto;
	else
		nameservers = service->nameservers;

	for (i = 0; nameservers != NULL && nameservers[i] != NULL; i++)
		if (g_strcmp0(nameservers[i], nameserver) == 0)
			return -EEXIST;

	if (nameservers != NULL) {
		len = g_strv_length(nameservers);
		nameservers = g_try_renew(char *, nameservers, len + 2);
	} else {
		len = 0;
		nameservers = g_try_new0(char *, len + 2);
	}

	if (nameservers == NULL)
		return -ENOMEM;

	nameservers[len] = g_strdup(nameserver);
	if (nameservers[len] == NULL)
		return -ENOMEM;

	nameservers[len + 1] = NULL;

	if (is_auto == TRUE) {
		service->nameservers_auto = nameservers;
	} else {
		service->nameservers = nameservers;
		update_nameservers(service);
	}

	return 0;
}

int __connman_service_nameserver_remove(struct connman_service *service,
				const char *nameserver, gboolean is_auto)
{
	char **servers, **nameservers;
	gboolean found = FALSE;
	int len, i, j;

	DBG("service %p nameserver %s auto %d", service, nameserver, is_auto);

	if (nameserver == NULL)
		return -EINVAL;

	if (is_auto == TRUE)
		nameservers = service->nameservers_auto;
	else
		nameservers = service->nameservers;

	if (nameservers == NULL)
		return 0;

	for (i = 0; nameservers != NULL && nameservers[i] != NULL; i++)
		if (g_strcmp0(nameservers[i], nameserver) == 0) {
			found = TRUE;
			break;
		}

	if (found == FALSE)
		return 0;

	len = g_strv_length(nameservers);

	if (len == 1) {
		g_strfreev(nameservers);
		if (is_auto == TRUE)
			service->nameservers_auto = NULL;
		else
			service->nameservers = NULL;

		return 0;
	}

	servers = g_try_new0(char *, len);
	if (servers == NULL)
		return -ENOMEM;

	for (i = 0, j = 0; i < len; i++) {
		if (g_strcmp0(nameservers[i], nameserver) != 0) {
			servers[j] = g_strdup(nameservers[i]);
			if (servers[j] == NULL)
				return -ENOMEM;
			j++;
		}
	}
	servers[len - 1] = NULL;

	g_strfreev(nameservers);
	nameservers = servers;

	if (is_auto == TRUE) {
		service->nameservers_auto = nameservers;
	} else {
		service->nameservers = nameservers;
		update_nameservers(service);
	}

	return 0;
}

void __connman_service_nameserver_clear(struct connman_service *service)
{
	g_strfreev(service->nameservers);
	service->nameservers = NULL;

	update_nameservers(service);
}

static void nameserver_add_routes(int index, char **nameservers,
					const char *gw)
{
	int i, ret, family;
	struct addrinfo hints;
	struct addrinfo *addr;

	for (i = 0; nameservers[i] != NULL; i++) {
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_flags = AI_NUMERICHOST;
		addr = NULL;

		ret = getaddrinfo(nameservers[i], NULL, &hints, &addr);
		if (ret == EAI_NONAME)
			family = AF_INET; /* use the IPv4 as a default */
		else if (ret != 0)
			continue;
		else
			family = addr->ai_family;

		if (family == AF_INET) {
			if (connman_inet_compare_subnet(index,
						nameservers[i]) != TRUE)
				connman_inet_add_host_route(index,
							nameservers[i], gw);
		} else if (family == AF_INET6)
			connman_inet_add_ipv6_host_route(index,
							nameservers[i], gw);

		freeaddrinfo(addr);
	}
}

static void nameserver_del_routes(int index, char **nameservers)
{
	int i, ret, family;
	struct addrinfo hints;
	struct addrinfo *addr;

	for (i = 0; nameservers[i] != NULL; i++) {
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_flags = AI_NUMERICHOST;
		addr = NULL;

		ret = getaddrinfo(nameservers[i], NULL, &hints, &addr);
		if (ret == EAI_NONAME)
			family = AF_INET; /* use the IPv4 as a default */
		else if (ret != 0)
			continue;
		else
			family = addr->ai_family;

		if (family == AF_INET)
			connman_inet_del_host_route(index, nameservers[i]);
		else if (family == AF_INET6)
			connman_inet_del_ipv6_host_route(index,
							nameservers[i]);

		freeaddrinfo(addr);
	}
}

void __connman_service_nameserver_add_routes(struct connman_service *service,
						const char *gw)
{
	int index = -1;

	if (service == NULL)
		return;

	if (service->network != NULL)
		index = connman_network_get_index(service->network);
	else if (service->provider != NULL)
		index = connman_provider_get_index(service->provider);

	if (service->nameservers_config != NULL) {
		/*
		 * Configured nameserver takes preference over the
		 * discoverd nameserver gathered from DHCP, VPN, etc.
		 */
		nameserver_add_routes(index, service->nameservers_config, gw);
	} else if (service->nameservers != NULL) {
		/*
		 * We add nameservers host routes for nameservers that
		 * are not on our subnet. For those who are, the subnet
		 * route will be installed by the time the dns proxy code
		 * tries to reach them. The subnet route is installed
		 * when setting the interface IP address.
		 */
		nameserver_add_routes(index, service->nameservers, gw);
	}
}

void __connman_service_nameserver_del_routes(struct connman_service *service)
{
	int index = -1;

	if (service == NULL)
		return;

	if (service->network != NULL)
		index = connman_network_get_index(service->network);
	else if (service->provider != NULL)
		index = connman_provider_get_index(service->provider);

	if (service->nameservers_config != NULL)
		nameserver_del_routes(index, service->nameservers_config);
	else if (service->nameservers != NULL)
		nameserver_del_routes(index, service->nameservers);
}

static struct connman_stats *stats_get(struct connman_service *service)
{
	if (service->roaming == TRUE)
		return &service->stats_roaming;
	else
		return &service->stats;
}

static connman_bool_t stats_enabled(struct connman_service *service)
{
	struct connman_stats *stats = stats_get(service);

	return stats->enabled;
}

static void stats_start(struct connman_service *service)
{
	struct connman_stats *stats = stats_get(service);

	DBG("service %p", service);

	if (stats->timer == NULL)
		return;

	stats->enabled = TRUE;
	stats->data_last.time = stats->data.time;

	g_timer_start(stats->timer);
}

static void stats_stop(struct connman_service *service)
{
	struct connman_stats *stats = stats_get(service);
	unsigned int seconds;

	DBG("service %p", service);

	if (stats->timer == NULL)
		return;

	if (stats->enabled == FALSE)
		return;

	g_timer_stop(stats->timer);

	seconds = g_timer_elapsed(stats->timer, NULL);
	stats->data.time = stats->data_last.time + seconds;

	stats->enabled = FALSE;
}

static void reset_stats(struct connman_service *service)
{
	DBG("service %p", service);

	/* home */
	service->stats.valid = FALSE;

	service->stats.data.rx_packets = 0;
	service->stats.data.tx_packets = 0;
	service->stats.data.rx_bytes = 0;
	service->stats.data.tx_bytes = 0;
	service->stats.data.rx_errors = 0;
	service->stats.data.tx_errors = 0;
	service->stats.data.rx_dropped = 0;
	service->stats.data.tx_dropped = 0;
	service->stats.data.time = 0;
	service->stats.data_last.time = 0;

	g_timer_reset(service->stats.timer);

	/* roaming */
	service->stats_roaming.valid = FALSE;

	service->stats_roaming.data.rx_packets = 0;
	service->stats_roaming.data.tx_packets = 0;
	service->stats_roaming.data.rx_bytes = 0;
	service->stats_roaming.data.tx_bytes = 0;
	service->stats_roaming.data.rx_errors = 0;
	service->stats_roaming.data.tx_errors = 0;
	service->stats_roaming.data.rx_dropped = 0;
	service->stats_roaming.data.tx_dropped = 0;
	service->stats_roaming.data.time = 0;
	service->stats_roaming.data_last.time = 0;

	g_timer_reset(service->stats_roaming.timer);
}

static struct connman_service *get_default(void)
{
	struct connman_service *service;
	GSequenceIter *iter;

	iter = g_sequence_get_begin_iter(service_list);

	if (g_sequence_iter_is_end(iter) == TRUE)
		return NULL;

	service = g_sequence_get(iter);

	if (is_connected(service) == FALSE)
		return NULL;

	return service;
}

static void default_changed(void)
{
	struct connman_service *service = get_default();

	__connman_notifier_default_changed(service);
}

const char *__connman_service_default(void)
{
	struct connman_service *service;

	service = get_default();
	if (service == NULL)
		return "";

	return __connman_service_type2string(service->type);
}

static void state_changed(struct connman_service *service)
{
	const char *str;

	__connman_notifier_service_state_changed(service, service->state);

	str = state2string(service->state);
	if (str == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "State",
						DBUS_TYPE_STRING, &str);
}

static void strength_changed(struct connman_service *service)
{
	if (service->strength == 0)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "Strength",
					DBUS_TYPE_BYTE, &service->strength);
}

static void favorite_changed(struct connman_service *service)
{
	if (service->path == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "Favorite",
					DBUS_TYPE_BOOLEAN, &service->favorite);
}

static void immutable_changed(struct connman_service *service)
{
	if (service->path == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "Immutable",
					DBUS_TYPE_BOOLEAN, &service->immutable);
}

static void roaming_changed(struct connman_service *service)
{
	if (service->path == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "Roaming",
					DBUS_TYPE_BOOLEAN, &service->roaming);
}

static void autoconnect_changed(struct connman_service *service)
{
	if (service->path == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "AutoConnect",
				DBUS_TYPE_BOOLEAN, &service->autoconnect);
}

static void passphrase_changed(struct connman_service *service)
{
	dbus_bool_t required;

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return;
	case CONNMAN_SERVICE_TYPE_WIFI:
		required = FALSE;

		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
			break;
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_PSK:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			if (service->passphrase == NULL)
				required = TRUE;
			break;
		case CONNMAN_SERVICE_SECURITY_8021X:
			break;
		}
		break;
	}

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "PassphraseRequired",
						DBUS_TYPE_BOOLEAN, &required);
}

static void login_changed(struct connman_service *service)
{
	dbus_bool_t required = service->login_required;

	if (service->path == NULL)
		return;

	connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "LoginRequired",
						DBUS_TYPE_BOOLEAN, &required);
}

static void append_security(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	const char *str;

	str = security2string(service->security);
	if (str != NULL)
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &str);

	/*
	 * Some access points incorrectly advertise WPS even when they
	 * are configured as open or no security, so filter
	 * appropriately.
	 */
	if (service->wps == TRUE) {
		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_PSK:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			str = "wps";
			dbus_message_iter_append_basic(iter,
			        DBUS_TYPE_STRING, &str);
			break;
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_8021X:
			break;
		}
	}
}

static void append_ethernet(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	if (service->ipconfig_ipv4 != NULL)
		__connman_ipconfig_append_ethernet(service->ipconfig_ipv4,
									iter);
	else if (service->ipconfig_ipv6 != NULL)
		__connman_ipconfig_append_ethernet(service->ipconfig_ipv6,
									iter);
}

static void append_ipv4(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	DBG("ipv4 %p state %s", service->ipconfig_ipv4,
				state2string(service->state_ipv4));

	if (is_connected_state(service, service->state_ipv4) == FALSE)
		return;

	if (service->ipconfig_ipv4 != NULL)
		__connman_ipconfig_append_ipv4(service->ipconfig_ipv4, iter);
}

static void append_ipv6(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	DBG("ipv6 %p state %s", service->ipconfig_ipv6,
				state2string(service->state_ipv6));

	if (is_connected_state(service, service->state_ipv6) == FALSE)
		return;

	if (service->ipconfig_ipv6 != NULL)
		__connman_ipconfig_append_ipv6(service->ipconfig_ipv6, iter,
						service->ipconfig_ipv4);
}

static void append_ipv4config(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	if (service->ipconfig_ipv4 != NULL)
		__connman_ipconfig_append_ipv4config(service->ipconfig_ipv4,
							iter);
}

static void append_ipv6config(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	if (service->ipconfig_ipv6 != NULL)
		__connman_ipconfig_append_ipv6config(service->ipconfig_ipv6,
							iter);
}

static void append_nameserver(DBusMessageIter *iter, char ***nameservers)
{
	char **servers;
	int i;

	servers = *nameservers;

	for (i = 0; servers[i] != NULL; i++) {
		dbus_message_iter_append_basic(iter,
					DBUS_TYPE_STRING, &servers[i]);
	}
}

static void append_dns(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	if (is_connected(service) == FALSE)
		return;

	if (service->nameservers_config != NULL) {
		append_nameserver(iter, &service->nameservers_config);
		return;
	} else {
		if (service->nameservers != NULL)
			append_nameserver(iter, &service->nameservers);

		if (service->nameservers_auto != NULL)
			append_nameserver(iter, &service->nameservers_auto);
	}
}

static void append_dnsconfig(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	int i;

	if (service->nameservers_config == NULL)
		return;

	for (i = 0; service->nameservers_config[i]; i++) {
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING,
				&service->nameservers_config[i]);
	}
}

static void append_domain(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	if (is_connected(service) == FALSE &&
				is_connecting(service) == FALSE)
		return;

	if (service->domainname == NULL)
		return;

	dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &service->domainname);
}

static void append_domainconfig(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	int i;

	if (service->domains == NULL)
		return;

	for (i = 0; service->domains[i]; i++)
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &service->domains[i]);
}

static void append_proxies(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	int i;

	if (service->proxies == NULL)
		return;

	for (i = 0; service->proxies[i]; i++)
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &service->proxies[i]);
}

static void append_excludes(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	int i;

	if (service->excludes == NULL)
		return;

	for (i = 0; service->excludes[i]; i++)
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &service->excludes[i]);
}

static void append_proxy(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	enum connman_service_proxy_method proxy;
	const char *pac = NULL;
	const char *method = proxymethod2string(
		CONNMAN_SERVICE_PROXY_METHOD_DIRECT);

	DBG("");

	if (is_connected(service) == FALSE)
		return;

	proxy = connman_service_get_proxy_method(service);

	switch (proxy) {
	case CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN:
		return;
	case CONNMAN_SERVICE_PROXY_METHOD_DIRECT:
		goto done;
	case CONNMAN_SERVICE_PROXY_METHOD_MANUAL:
		connman_dbus_dict_append_array(iter, "Servers",
					DBUS_TYPE_STRING, append_proxies,
					service);

		connman_dbus_dict_append_array(iter, "Excludes",
					DBUS_TYPE_STRING, append_excludes,
					service);
		break;
	case CONNMAN_SERVICE_PROXY_METHOD_AUTO:
		/* Maybe DHCP, or WPAD,  has provided an url for a pac file */
		if (service->ipconfig_ipv4 != NULL)
			pac = __connman_ipconfig_get_proxy_autoconfig(
				service->ipconfig_ipv4);
		else if (service->ipconfig_ipv6 != NULL)
			pac = __connman_ipconfig_get_proxy_autoconfig(
				service->ipconfig_ipv6);

		if (service->pac == NULL && pac == NULL)
			goto done;

		if (service->pac != NULL)
			pac = service->pac;

		connman_dbus_dict_append_basic(iter, "URL",
					DBUS_TYPE_STRING, &pac);
		break;
	}

	method = proxymethod2string(proxy);

done:
	connman_dbus_dict_append_basic(iter, "Method",
					DBUS_TYPE_STRING, &method);
}

static void append_proxyconfig(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	const char *method;

	if (service->proxy_config == CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN)
		return;

	switch (service->proxy_config) {
	case CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN:
		return;
	case CONNMAN_SERVICE_PROXY_METHOD_DIRECT:
		break;
	case CONNMAN_SERVICE_PROXY_METHOD_MANUAL:
		if (service->proxies != NULL)
			connman_dbus_dict_append_array(iter, "Servers",
						DBUS_TYPE_STRING,
						append_proxies, service);

		if (service->excludes != NULL)
			connman_dbus_dict_append_array(iter, "Excludes",
						DBUS_TYPE_STRING,
						append_excludes, service);
		break;
	case CONNMAN_SERVICE_PROXY_METHOD_AUTO:
		if (service->pac != NULL)
			connman_dbus_dict_append_basic(iter, "URL",
					DBUS_TYPE_STRING, &service->pac);
		break;
	}

	method = proxymethod2string(service->proxy_config);

	connman_dbus_dict_append_basic(iter, "Method",
				DBUS_TYPE_STRING, &method);
}

static void append_provider(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;

	DBG("%p %p", service, service->provider);

	if (is_connected(service) == FALSE)
		return;

	if (service->provider != NULL)
		__connman_provider_append_properties(service->provider, iter);
}


static void settings_changed(struct connman_service *service,
				struct connman_ipconfig *ipconfig)
{
	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE, "IPv4",
							append_ipv4, service);

	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE, "IPv6",
							append_ipv6, service);

	__connman_notifier_ipconfig_changed(service, ipconfig);
}

static void ipv4_configuration_changed(struct connman_service *service)
{
	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE,
							"IPv4.Configuration",
							append_ipv4config,
							service);
}

static void ipv6_configuration_changed(struct connman_service *service)
{
	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE,
							"IPv6.Configuration",
							append_ipv6config,
							service);
}

static void dns_changed(struct connman_service *service)
{
	connman_dbus_property_changed_array(service->path,
				CONNMAN_SERVICE_INTERFACE, "Nameservers",
					DBUS_TYPE_STRING, append_dns, service);
}

static void dns_configuration_changed(struct connman_service *service)
{
	connman_dbus_property_changed_array(service->path,
				CONNMAN_SERVICE_INTERFACE,
				"Nameservers.Configuration",
				DBUS_TYPE_STRING, append_dnsconfig, service);

	dns_changed(service);
}

static void domain_changed(struct connman_service *service)
{
	connman_dbus_property_changed_array(service->path,
				CONNMAN_SERVICE_INTERFACE, "Domains",
				DBUS_TYPE_STRING, append_domain, service);
}

static void domain_configuration_changed(struct connman_service *service)
{
	connman_dbus_property_changed_array(service->path,
				CONNMAN_SERVICE_INTERFACE,
				"Domains.Configuration",
				DBUS_TYPE_STRING, append_domainconfig, service);
}

static void proxy_changed(struct connman_service *service)
{
	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE, "Proxy",
							append_proxy, service);
}

static void proxy_configuration_changed(struct connman_service *service)
{
	connman_dbus_property_changed_dict(service->path,
			CONNMAN_SERVICE_INTERFACE, "Proxy.Configuration",
						append_proxyconfig, service);

	proxy_changed(service);
}

static void link_changed(struct connman_service *service)
{
	connman_dbus_property_changed_dict(service->path,
					CONNMAN_SERVICE_INTERFACE, "Ethernet",
						append_ethernet, service);
}

static void stats_append_counters(DBusMessageIter *dict,
			struct connman_stats_data *stats,
			struct connman_stats_data *counters,
			connman_bool_t append_all)
{
	if (counters->rx_packets != stats->rx_packets || append_all) {
		counters->rx_packets = stats->rx_packets;
		connman_dbus_dict_append_basic(dict, "RX.Packets",
					DBUS_TYPE_UINT32, &stats->rx_packets);
	}

	if (counters->tx_packets != stats->tx_packets || append_all) {
		counters->tx_packets = stats->tx_packets;
		connman_dbus_dict_append_basic(dict, "TX.Packets",
					DBUS_TYPE_UINT32, &stats->tx_packets);
	}

	if (counters->rx_bytes != stats->rx_bytes || append_all) {
		counters->rx_bytes = stats->rx_bytes;
		connman_dbus_dict_append_basic(dict, "RX.Bytes",
					DBUS_TYPE_UINT32, &stats->rx_bytes);
	}

	if (counters->tx_bytes != stats->tx_bytes || append_all) {
		counters->tx_bytes = stats->tx_bytes;
		connman_dbus_dict_append_basic(dict, "TX.Bytes",
					DBUS_TYPE_UINT32, &stats->tx_bytes);
	}

	if (counters->rx_errors != stats->rx_errors || append_all) {
		counters->rx_errors = stats->rx_errors;
		connman_dbus_dict_append_basic(dict, "RX.Errors",
					DBUS_TYPE_UINT32, &stats->rx_errors);
	}

	if (counters->tx_errors != stats->tx_errors || append_all) {
		counters->tx_errors = stats->tx_errors;
		connman_dbus_dict_append_basic(dict, "TX.Errors",
					DBUS_TYPE_UINT32, &stats->tx_errors);
	}

	if (counters->rx_dropped != stats->rx_dropped || append_all) {
		counters->rx_dropped = stats->rx_dropped;
		connman_dbus_dict_append_basic(dict, "RX.Dropped",
					DBUS_TYPE_UINT32, &stats->rx_dropped);
	}

	if (counters->tx_dropped != stats->tx_dropped || append_all) {
		counters->tx_dropped = stats->tx_dropped;
		connman_dbus_dict_append_basic(dict, "TX.Dropped",
					DBUS_TYPE_UINT32, &stats->tx_dropped);
	}

	if (counters->time != stats->time || append_all) {
		counters->time = stats->time;
		connman_dbus_dict_append_basic(dict, "Time",
					DBUS_TYPE_UINT32, &stats->time);
	}
}

static void stats_append(struct connman_service *service,
				const char *counter,
				struct connman_stats_counter *counters,
				connman_bool_t append_all)
{
	DBusMessageIter array, dict;
	DBusMessage *msg;

	DBG("service %p counter %s", service, counter);

	msg = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_CALL);
	if (msg == NULL)
		return;

	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH,
				&service->path, DBUS_TYPE_INVALID);

	dbus_message_iter_init_append(msg, &array);

	/* home counter */
	connman_dbus_dict_open(&array, &dict);

	stats_append_counters(&dict, &service->stats.data,
				&counters->stats.data, append_all);

	connman_dbus_dict_close(&array, &dict);

	/* roaming counter */
	connman_dbus_dict_open(&array, &dict);

	stats_append_counters(&dict, &service->stats_roaming.data,
				&counters->stats_roaming.data, append_all);

	connman_dbus_dict_close(&array, &dict);

	__connman_counter_send_usage(counter, msg);
}

static void stats_update(struct connman_service *service,
				unsigned int rx_packets, unsigned int tx_packets,
				unsigned int rx_bytes, unsigned int tx_bytes,
				unsigned int rx_errors, unsigned int tx_errors,
				unsigned int rx_dropped, unsigned int tx_dropped)
{
	struct connman_stats *stats = stats_get(service);
	struct connman_stats_data *data_last = &stats->data_last;
	struct connman_stats_data *data = &stats->data;
	unsigned int seconds;

	DBG("service %p", service);

	if (stats->valid == TRUE) {
		data->rx_packets +=
			rx_packets - data_last->rx_packets;
		data->tx_packets +=
			tx_packets - data_last->tx_packets;
		data->rx_bytes +=
			rx_bytes - data_last->rx_bytes;
		data->tx_bytes +=
			tx_bytes - data_last->tx_bytes;
		data->rx_errors +=
			rx_errors - data_last->rx_errors;
		data->tx_errors +=
			tx_errors - data_last->tx_errors;
		data->rx_dropped +=
			rx_dropped - data_last->rx_dropped;
		data->tx_dropped +=
			tx_dropped - data_last->tx_dropped;
	} else {
		stats->valid = TRUE;
	}

	data_last->rx_packets = rx_packets;
	data_last->tx_packets = tx_packets;
	data_last->rx_bytes = rx_bytes;
	data_last->tx_bytes = tx_bytes;
	data_last->rx_errors = rx_errors;
	data_last->tx_errors = tx_errors;
	data_last->rx_dropped = rx_dropped;
	data_last->tx_dropped = tx_dropped;

	seconds = g_timer_elapsed(stats->timer, NULL);
	stats->data.time = stats->data_last.time + seconds;
}

static char *wifi_build_group_name(const unsigned char *ssid,
						unsigned int ssid_len,
							const char *mode,
							const char *security)
{
	GString *str;
	unsigned int i;

	/* the last 3 is for the 2 '_' and '\0' */
	str = g_string_sized_new((ssid_len * 2) + strlen(mode)
					+ strlen(security) + 3);
	if (str == NULL)
		return NULL;

	for (i = 0; i < ssid_len; i++)
		g_string_append_printf(str, "%02x", ssid[i]);

	g_string_append_printf(str, "_%s_%s", mode, security);

	return g_string_free(str, FALSE);
}

void __connman_service_notify(struct connman_service *service,
			unsigned int rx_packets, unsigned int tx_packets,
			unsigned int rx_bytes, unsigned int tx_bytes,
			unsigned int rx_errors, unsigned int tx_errors,
			unsigned int rx_dropped, unsigned int tx_dropped)
{
	GHashTableIter iter;
	gpointer key, value;
	const char *counter;
	struct connman_stats_counter *counters;
	struct connman_stats_data *data;
	int err;

	if (service == NULL)
		return;

	if (is_connected(service) == FALSE)
		return;

	stats_update(service,
		rx_packets, tx_packets,
		rx_bytes, tx_bytes,
		rx_errors, tx_errors,
		rx_dropped, tx_dropped);

	data = &stats_get(service)->data;
	err = __connman_stats_update(service, service->roaming, data);
	if (err < 0)
		connman_error("Failed to store statistics for %s",
				service->identifier);

	g_hash_table_iter_init(&iter, service->counter_table);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		counter = key;
		counters = value;

		stats_append(service, counter, counters, counters->append_all);
		counters->append_all = FALSE;
	}
}

int __connman_service_counter_register(const char *counter)
{
	struct connman_service *service;
	GSequenceIter *iter;
	struct connman_stats_counter *counters;

	DBG("counter %s", counter);

	counter_list = g_slist_append(counter_list, (gpointer)counter);

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		service = g_sequence_get(iter);

		counters = g_try_new0(struct connman_stats_counter, 1);
		if (counters == NULL)
			return -ENOMEM;

		counters->append_all = TRUE;

		g_hash_table_replace(service->counter_table, (gpointer)counter,
					counters);

		iter = g_sequence_iter_next(iter);
	}

	return 0;
}

void __connman_service_counter_unregister(const char *counter)
{
	struct connman_service *service;
	GSequenceIter *iter;

	DBG("counter %s", counter);

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		service = g_sequence_get(iter);

		g_hash_table_remove(service->counter_table, counter);

		iter = g_sequence_iter_next(iter);
	}

	counter_list = g_slist_remove(counter_list, counter);
}

GSequence *__connman_service_get_list(struct connman_session *session,
				service_match_cb service_match,
				create_service_entry_cb create_service_entry,
				GDestroyNotify destroy_service_entry)
{
	GSequence *list;
	GSequenceIter *iter;
	struct connman_service *service;
	struct service_entry *entry;

	list = g_sequence_new(destroy_service_entry);
	if (list == NULL)
		return NULL;

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		service = g_sequence_get(iter);

		if (service_match(session, service) == TRUE) {
			entry = create_service_entry(service, service->name,
							service->state);
			if (entry == NULL)
				return list;

			g_sequence_append(list, entry);
		}

		iter = g_sequence_iter_next(iter);
	}

	return list;
}

void __connman_service_session_inc(struct connman_service *service)
{
	DBG("service %p ref count %d", service,
		service->session_usage_count + 1);

	__sync_fetch_and_add(&service->session_usage_count, 1);
}

connman_bool_t __connman_service_session_dec(struct connman_service *service)
{
	DBG("service %p ref count %d", service,
		service->session_usage_count - 1);

	if (__sync_fetch_and_sub(&service->session_usage_count, 1) != 1)
		return FALSE;

	return TRUE;
}

static void append_properties(DBusMessageIter *dict, dbus_bool_t limited,
					struct connman_service *service)
{
	dbus_bool_t required;
	const char *str;

	str = __connman_service_type2string(service->type);
	if (str != NULL)
		connman_dbus_dict_append_basic(dict, "Type",
						DBUS_TYPE_STRING, &str);

	connman_dbus_dict_append_array(dict, "Security",
				DBUS_TYPE_STRING, append_security, service);

	str = state2string(service->state);
	if (str != NULL)
		connman_dbus_dict_append_basic(dict, "State",
						DBUS_TYPE_STRING, &str);

	str = error2string(service->error);
	if (str != NULL)
		connman_dbus_dict_append_basic(dict, "Error",
						DBUS_TYPE_STRING, &str);

	if (service->strength > 0)
		connman_dbus_dict_append_basic(dict, "Strength",
					DBUS_TYPE_BYTE, &service->strength);

	connman_dbus_dict_append_basic(dict, "Favorite",
					DBUS_TYPE_BOOLEAN, &service->favorite);

	connman_dbus_dict_append_basic(dict, "Immutable",
					DBUS_TYPE_BOOLEAN, &service->immutable);

	if (service->favorite == TRUE)
		connman_dbus_dict_append_basic(dict, "AutoConnect",
				DBUS_TYPE_BOOLEAN, &service->autoconnect);
	else
		connman_dbus_dict_append_basic(dict, "AutoConnect",
					DBUS_TYPE_BOOLEAN, &service->favorite);

	if (service->name != NULL)
		connman_dbus_dict_append_basic(dict, "Name",
					DBUS_TYPE_STRING, &service->name);

	connman_dbus_dict_append_basic(dict, "LoginRequired",
				DBUS_TYPE_BOOLEAN, &service->login_required);

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		break;
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		connman_dbus_dict_append_basic(dict, "Roaming",
					DBUS_TYPE_BOOLEAN, &service->roaming);

		connman_dbus_dict_append_dict(dict, "Ethernet",
						append_ethernet, service);
		break;
	case CONNMAN_SERVICE_TYPE_WIFI:
		if (service->passphrase != NULL && limited == FALSE)
			connman_dbus_dict_append_basic(dict, "Passphrase",
				DBUS_TYPE_STRING, &service->passphrase);

		required = FALSE;

		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
			break;
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_PSK:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			if (service->passphrase == NULL)
				required = TRUE;
			break;
		case CONNMAN_SERVICE_SECURITY_8021X:
			break;
		}

		connman_dbus_dict_append_basic(dict, "PassphraseRequired",
						DBUS_TYPE_BOOLEAN, &required);

		/* fall through */
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
		connman_dbus_dict_append_dict(dict, "Ethernet",
						append_ethernet, service);
		break;
	}

	connman_dbus_dict_append_dict(dict, "IPv4", append_ipv4, service);

	connman_dbus_dict_append_dict(dict, "IPv4.Configuration",
						append_ipv4config, service);

	connman_dbus_dict_append_dict(dict, "IPv6", append_ipv6, service);

	connman_dbus_dict_append_dict(dict, "IPv6.Configuration",
						append_ipv6config, service);

	connman_dbus_dict_append_array(dict, "Nameservers",
				DBUS_TYPE_STRING, append_dns, service);

	connman_dbus_dict_append_array(dict, "Nameservers.Configuration",
				DBUS_TYPE_STRING, append_dnsconfig, service);

	connman_dbus_dict_append_array(dict, "Domains",
				DBUS_TYPE_STRING, append_domain, service);

	connman_dbus_dict_append_array(dict, "Domains.Configuration",
				DBUS_TYPE_STRING, append_domainconfig, service);

	connman_dbus_dict_append_dict(dict, "Proxy", append_proxy, service);

	connman_dbus_dict_append_dict(dict, "Proxy.Configuration",
						append_proxyconfig, service);

	connman_dbus_dict_append_dict(dict, "Provider",
						append_provider, service);
}

static void append_struct(gpointer value, gpointer user_data)
{
	struct connman_service *service = value;
	DBusMessageIter *iter = user_data;
	DBusMessageIter entry, dict;

	if (service->path == NULL || service->hidden == TRUE)
		return;

	dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
							&service->path);

	connman_dbus_dict_open(&entry, &dict);
	append_properties(&dict, TRUE, service);
	connman_dbus_dict_close(&entry, &dict);

	dbus_message_iter_close_container(iter, &entry);
}

void __connman_service_list_struct(DBusMessageIter *iter)
{
	g_sequence_foreach(service_list, append_struct, iter);
}

int __connman_service_get_index(struct connman_service *service)
{
	if (service == NULL)
		return -1;

	if (service->network != NULL)
		return connman_network_get_index(service->network);
	else if (service->provider != NULL)
		return connman_provider_get_index(service->provider);

	return -1;
}

void __connman_service_set_domainname(struct connman_service *service,
						const char *domainname)
{
	if (service == NULL)
		return;

	g_free(service->domainname);
	service->domainname = g_strdup(domainname);

	domain_changed(service);
}

const char *connman_service_get_domainname(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	if (service->domains != NULL)
		return service->domains[0];
	else
		return service->domainname;
}

char **connman_service_get_nameservers(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	if (service->nameservers_config != NULL)
		return service->nameservers_config;
	else if (service->nameservers != NULL)
		return service->nameservers;

	return NULL;
}

void connman_service_set_proxy_method(struct connman_service *service,
					enum connman_service_proxy_method method)
{
	if (service == NULL)
		return;

	service->proxy = method;

	proxy_changed(service);

	if (method != CONNMAN_SERVICE_PROXY_METHOD_AUTO)
		__connman_notifier_proxy_changed(service);
}

enum connman_service_proxy_method connman_service_get_proxy_method(
					struct connman_service *service)
{
	if (service == NULL)
		return CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN;

	if (service->proxy_config != CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN) {
		if (service->proxy_config == CONNMAN_SERVICE_PROXY_METHOD_AUTO &&
				service->pac == NULL)
			return service->proxy;

		return service->proxy_config;
	}

	return service->proxy;
}

char **connman_service_get_proxy_servers(struct connman_service *service)
{
	return g_strdupv(service->proxies);
}

char **connman_service_get_proxy_excludes(struct connman_service *service)
{
	return g_strdupv(service->excludes);
}

const char *connman_service_get_proxy_url(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	return service->pac;
}

void __connman_service_set_proxy_autoconfig(struct connman_service *service,
							const char *url)
{
	if (service == NULL)
		return;

	service->proxy = CONNMAN_SERVICE_PROXY_METHOD_AUTO;

	if (service->ipconfig_ipv4) {
		if (__connman_ipconfig_set_proxy_autoconfig(
			    service->ipconfig_ipv4, url) < 0)
			return;
	} else if (service->ipconfig_ipv6) {
		if (__connman_ipconfig_set_proxy_autoconfig(
			    service->ipconfig_ipv6, url) < 0)
			return;
	} else
		return;

	proxy_changed(service);

	__connman_notifier_proxy_changed(service);
}

const char *connman_service_get_proxy_autoconfig(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	if (service->ipconfig_ipv4)
		return __connman_ipconfig_get_proxy_autoconfig(
						service->ipconfig_ipv4);
	else if (service->ipconfig_ipv6)
		return __connman_ipconfig_get_proxy_autoconfig(
						service->ipconfig_ipv6);
	return NULL;
}

static void update_timeservers(struct connman_service *service)
{
	int i;

	if (service->timeservers == NULL)
		return;

	switch (service->state) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
	case CONNMAN_SERVICE_STATE_IDLE:
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
		return;
	case CONNMAN_SERVICE_STATE_FAILURE:
	case CONNMAN_SERVICE_STATE_DISCONNECT:
		for (i = 0; service->timeservers[i] != NULL; i++)
			connman_timeserver_remove(service->timeservers[i]);
		return;
	case CONNMAN_SERVICE_STATE_READY:
	case CONNMAN_SERVICE_STATE_ONLINE:
		break;
	}

	for (i = 0; service->timeservers[i] != NULL; i++)
		connman_timeserver_append(service->timeservers[i]);
}

int __connman_service_timeserver_append(struct connman_service *service,
						const char *timeserver)
{
	int len;

	DBG("service %p timeserver %s", service, timeserver);

	if (timeserver == NULL)
		return -EINVAL;

	if (service->timeservers != NULL) {
		int i;

		for (i = 0; service->timeservers[i] != NULL; i++)
			if (g_strcmp0(service->timeservers[i], timeserver) == 0)
				return -EEXIST;

		len = g_strv_length(service->timeservers);
		service->timeservers = g_try_renew(char *, service->timeservers,
							len + 2);
	} else {
		len = 0;
		service->timeservers = g_try_new0(char *, len + 2);
	}

	if (service->timeservers == NULL)
		return -ENOMEM;

	service->timeservers[len] = g_strdup(timeserver);
	service->timeservers[len + 1] = NULL;

	update_timeservers(service);

	return 0;
}

int __connman_service_timeserver_remove(struct connman_service *service,
						const char *timeserver)
{
	char **servers;
	int len, i, j;

	DBG("service %p timeserver %s", service, timeserver);

	if (timeserver == NULL)
		return -EINVAL;

	if (service->timeservers == NULL)
		return 0;

	len = g_strv_length(service->timeservers);
	if (len == 1) {
		if (g_strcmp0(service->timeservers[0], timeserver) != 0)
			return 0;

		g_strfreev(service->timeservers);
		service->timeservers = NULL;

		return 0;
	}

	servers = g_try_new0(char *, len - 1);
	if (servers == NULL)
		return -ENOMEM;

	for (i = 0, j = 0; i < len; i++) {
		if (g_strcmp0(service->timeservers[i], timeserver) != 0) {
			servers[j] = g_strdup(service->timeservers[i]);
			j++;
		}
	}
	servers[len - 2] = NULL;

	g_strfreev(service->timeservers);
	service->timeservers = servers;

	update_timeservers(service);

	return 0;
}

void __connman_service_set_pac(struct connman_service *service,
					const char *pac)
{
	g_free(service->pac);
	service->pac = g_strdup(pac);

	proxy_changed(service);
}

void __connman_service_set_identity(struct connman_service *service,
					const char *identity)
{
	if (service->immutable)
		return;

	g_free(service->identity);
	service->identity = g_strdup(identity);

	if (service->network != NULL)
		connman_network_set_string(service->network,
					"WiFi.Identity",
					service->identity);
}

void __connman_service_set_agent_identity(struct connman_service *service,
						const char *agent_identity)
{
	g_free(service->agent_identity);
	service->agent_identity = g_strdup(agent_identity);

	if (service->network != NULL)
		connman_network_set_string(service->network,
					"WiFi.AgentIdentity",
					service->agent_identity);
}

void __connman_service_set_passphrase(struct connman_service *service,
					const char* passphrase)
{
	if (service->immutable == TRUE)
		return;

	g_free(service->passphrase);
	service->passphrase = g_strdup(passphrase);

	passphrase_changed(service);

	if (service->network != NULL)
		connman_network_set_string(service->network,
					"WiFi.Passphrase",
					service->passphrase);

	service_save(service);
}

void __connman_service_set_agent_passphrase(struct connman_service *service,
						const char *agent_passphrase)
{
	g_free(service->agent_passphrase);
	service->agent_passphrase = g_strdup(agent_passphrase);

	if (service->network != NULL)
		connman_network_set_string(service->network,
					"WiFi.AgentPassphrase",
					service->agent_passphrase);
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;
	DBusMessage *reply;
	DBusMessageIter array, dict;

	DBG("service %p", service);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &array);

	connman_dbus_dict_open(&array, &dict);
	append_properties(&dict, FALSE, service);
	connman_dbus_dict_close(&array, &dict);

	return reply;
}

static int update_proxy_configuration(struct connman_service *service,
				DBusMessageIter *array)
{
	DBusMessageIter dict;
	enum connman_service_proxy_method method;
	GString *servers_str = NULL;
	GString *excludes_str = NULL;
	const char *url = NULL;

	method = CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN;

	dbus_message_iter_recurse(array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *key;
		int type;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto error;

		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) !=
							DBUS_TYPE_VARIANT)
			goto error;

		dbus_message_iter_recurse(&entry, &variant);
		type = dbus_message_iter_get_arg_type(&variant);

		if (g_str_equal(key, "Method") == TRUE) {
			const char *val;

			if (type != DBUS_TYPE_STRING)
				goto error;

			dbus_message_iter_get_basic(&variant, &val);
			method = string2proxymethod(val);
		} else if (g_str_equal(key, "URL") == TRUE) {
			if (type != DBUS_TYPE_STRING)
				goto error;

			dbus_message_iter_get_basic(&variant, &url);
		} else if (g_str_equal(key, "Servers") == TRUE) {
			DBusMessageIter str_array;

			if (type != DBUS_TYPE_ARRAY)
				goto error;

			servers_str = g_string_new(NULL);
			if (servers_str == NULL)
				goto error;

			dbus_message_iter_recurse(&variant, &str_array);

			while (dbus_message_iter_get_arg_type(&str_array) ==
							DBUS_TYPE_STRING) {
				char *val = NULL;

				dbus_message_iter_get_basic(&str_array, &val);

				if (servers_str->len > 0)
					g_string_append_printf(servers_str,
							" %s", val);
				else
					g_string_append(servers_str, val);

				dbus_message_iter_next(&str_array);
			}
		} else if (g_str_equal(key, "Excludes") == TRUE) {
			DBusMessageIter str_array;

			if (type != DBUS_TYPE_ARRAY)
				goto error;

			excludes_str = g_string_new(NULL);
			if (excludes_str == NULL)
				goto error;

			dbus_message_iter_recurse(&variant, &str_array);

			while (dbus_message_iter_get_arg_type(&str_array) ==
							DBUS_TYPE_STRING) {
				char *val = NULL;

				dbus_message_iter_get_basic(&str_array, &val);

				if (excludes_str->len > 0)
					g_string_append_printf(excludes_str,
							" %s", val);
				else
					g_string_append(excludes_str, val);

				dbus_message_iter_next(&str_array);
			}
		}

		dbus_message_iter_next(&dict);
	}

	switch (method) {
	case CONNMAN_SERVICE_PROXY_METHOD_DIRECT:
		break;
	case CONNMAN_SERVICE_PROXY_METHOD_MANUAL:
		if (servers_str == NULL && service->proxies == NULL)
			goto error;

		if (servers_str != NULL) {
			g_strfreev(service->proxies);

			if (servers_str->len > 0)
				service->proxies = g_strsplit_set(
					servers_str->str, " ", 0);
			else
				service->proxies = NULL;
		}

		if (excludes_str != NULL) {
			g_strfreev(service->excludes);

			if (excludes_str->len > 0)
				service->excludes = g_strsplit_set(
					excludes_str->str, " ", 0);
			else
				service->excludes = NULL;
		}

		if (service->proxies == NULL)
			method = CONNMAN_SERVICE_PROXY_METHOD_DIRECT;

		break;
	case CONNMAN_SERVICE_PROXY_METHOD_AUTO:
		g_free(service->pac);

		if (url != NULL && strlen(url) > 0)
			service->pac = g_strdup(url);
		else
			service->pac = NULL;

		/* if we are connected:
		   - if service->pac == NULL
		   - if __connman_ipconfig_get_proxy_autoconfig(
		   service->ipconfig) == NULL
		   --> We should start WPAD */

		break;
	case CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN:
		goto error;
	}

	if (servers_str != NULL)
		g_string_free(servers_str, TRUE);

	if (excludes_str != NULL)
		g_string_free(excludes_str, TRUE);

	service->proxy_config = method;

	return 0;

error:
	if (servers_str != NULL)
		g_string_free(servers_str, TRUE);

	if (excludes_str != NULL)
		g_string_free(excludes_str, TRUE);

	return -EINVAL;
}

static int set_ipconfig(struct connman_service *service,
			struct connman_ipconfig *ipconfig,
			DBusMessageIter *array,
			enum connman_service_state state,
			enum connman_service_state *new_state)
{
	enum connman_ipconfig_method old_method;
	enum connman_ipconfig_method method = CONNMAN_IPCONFIG_METHOD_UNKNOWN;
	enum connman_ipconfig_type type;
	int err;

	if (ipconfig == NULL)
		return -EINVAL;

	old_method = __connman_ipconfig_get_method(ipconfig);

	if (is_connecting_state(service, state) ||
					is_connected_state(service, state))
		__connman_network_clear_ipconfig(service->network, ipconfig);

	err = __connman_ipconfig_set_config(ipconfig, array);
	method = __connman_ipconfig_get_method(ipconfig);
	type = __connman_ipconfig_get_config_type(ipconfig);

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		if (err == 0 && old_method == CONNMAN_IPCONFIG_METHOD_OFF &&
				method == CONNMAN_IPCONFIG_METHOD_DHCP) {
			*new_state = service->state_ipv4 =
				CONNMAN_SERVICE_STATE_CONFIGURATION;
			__connman_ipconfig_enable(ipconfig);
		}

	} else if (type == CONNMAN_IPCONFIG_TYPE_IPV6) {
		if (err == 0 && old_method == CONNMAN_IPCONFIG_METHOD_OFF &&
				method == CONNMAN_IPCONFIG_METHOD_AUTO) {
			*new_state = service->state_ipv6;
			__connman_ipconfig_enable(ipconfig);
		}
	}

	DBG("err %d ipconfig %p type %d method %d state %s", err, ipconfig,
		type, method, state2string(*new_state));

	return err;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;
	DBusMessageIter iter, value;
	const char *name;
	int type;

	DBG("service %p", service);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __connman_error_invalid_arguments(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	type = dbus_message_iter_get_arg_type(&value);

	if (g_str_equal(name, "AutoConnect") == TRUE) {
		connman_bool_t autoconnect;

		if (type != DBUS_TYPE_BOOLEAN)
			return __connman_error_invalid_arguments(msg);

		if (service->favorite == FALSE)
			return __connman_error_invalid_service(msg);

		dbus_message_iter_get_basic(&value, &autoconnect);

		if (service->autoconnect == autoconnect)
			return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);

		service->autoconnect = autoconnect;

		autoconnect_changed(service);

		service_save(service);
	} else if (g_str_equal(name, "Passphrase") == TRUE) {
		const char *passphrase;

		if (type != DBUS_TYPE_STRING)
			return __connman_error_invalid_arguments(msg);

		if (service->immutable == TRUE)
			return __connman_error_not_supported(msg);

		dbus_message_iter_get_basic(&value, &passphrase);

		__connman_service_set_passphrase(service, passphrase);
	} else if (g_str_equal(name, "Nameservers.Configuration") == TRUE) {
		DBusMessageIter entry;
		GString *str;
		int index;
		const char *gw;

		if (type != DBUS_TYPE_ARRAY)
			return __connman_error_invalid_arguments(msg);

		str = g_string_new(NULL);
		if (str == NULL)
			return __connman_error_invalid_arguments(msg);

		index = connman_network_get_index(service->network);
		gw = __connman_ipconfig_get_gateway_from_index(index);

		if (gw && strlen(gw))
			__connman_service_nameserver_del_routes(service);

		dbus_message_iter_recurse(&value, &entry);

		while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
			const char *val;
			dbus_message_iter_get_basic(&entry, &val);
			dbus_message_iter_next(&entry);
			if (str->len > 0)
				g_string_append_printf(str, " %s", val);
			else
				g_string_append(str, val);
		}

		g_strfreev(service->nameservers_config);

		if (str->len > 0) {
			service->nameservers_config =
				g_strsplit_set(str->str, " ", 0);
		} else {
			service->nameservers_config = NULL;
		}

		g_string_free(str, TRUE);

		if (gw && strlen(gw))
			__connman_service_nameserver_add_routes(service, gw);

		update_nameservers(service);
		dns_configuration_changed(service);

		service_save(service);
	} else if (g_str_equal(name, "Domains.Configuration") == TRUE) {
		DBusMessageIter entry;
		GString *str;

		if (type != DBUS_TYPE_ARRAY)
			return __connman_error_invalid_arguments(msg);

		str = g_string_new(NULL);
		if (str == NULL)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_recurse(&value, &entry);

		while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
			const char *val;
			dbus_message_iter_get_basic(&entry, &val);
			dbus_message_iter_next(&entry);
			if (str->len > 0)
				g_string_append_printf(str, " %s", val);
			else
				g_string_append(str, val);
		}

		g_strfreev(service->domains);

		if (str->len > 0)
			service->domains = g_strsplit_set(str->str, " ", 0);
		else
			service->domains = NULL;

		g_string_free(str, TRUE);

		update_nameservers(service);
		domain_configuration_changed(service);

		service_save(service);
	} else if (g_str_equal(name, "Proxy.Configuration") == TRUE) {
		int err;

		if (type != DBUS_TYPE_ARRAY)
			return __connman_error_invalid_arguments(msg);

		err = update_proxy_configuration(service, &value);

		if (err < 0)
			return __connman_error_failed(msg, -err);

		proxy_configuration_changed(service);

		__connman_notifier_proxy_changed(service);

		service_save(service);
	} else if (g_str_equal(name, "IPv4.Configuration") == TRUE ||
			g_str_equal(name, "IPv6.Configuration")) {

		struct connman_ipconfig *ipv4 = NULL, *ipv6 = NULL;
		enum connman_service_state state =
						CONNMAN_SERVICE_STATE_UNKNOWN;
		int err = 0;

		DBG("%s", name);

		if (service->ipconfig_ipv4 == NULL &&
					service->ipconfig_ipv6 == NULL)
			return __connman_error_invalid_property(msg);

		if (g_str_equal(name, "IPv4.Configuration") == TRUE) {
			ipv4 = service->ipconfig_ipv4;
			err = set_ipconfig(service, ipv4, &value,
					service->state_ipv4, &state);

		} else if (g_str_equal(name, "IPv6.Configuration") == TRUE) {
			ipv6 = service->ipconfig_ipv6;
			err = set_ipconfig(service, ipv6, &value,
					service->state_ipv6, &state);
		}

		if (err < 0) {
			if (is_connected_state(service, state) ||
					is_connecting_state(service, state))
				__connman_network_set_ipconfig(service->network,
								ipv4, ipv6);
			return __connman_error_failed(msg, -err);
		}

		if (ipv4)
			ipv4_configuration_changed(service);
		else if (ipv6)
			ipv6_configuration_changed(service);

		if (is_connecting(service) || is_connected(service))
			__connman_network_set_ipconfig(service->network,
							ipv4, ipv6);

		service_save(service);
	} else
		return __connman_error_invalid_property(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static void set_idle(struct connman_service *service)
{
	service->state = service->state_ipv4 = service->state_ipv6 =
						CONNMAN_SERVICE_STATE_IDLE;
	service->error = CONNMAN_SERVICE_ERROR_UNKNOWN;
	state_changed(service);
}

static DBusMessage *clear_property(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;
	const char *name;

	DBG("service %p", service);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name,
							DBUS_TYPE_INVALID);

	if (g_str_equal(name, "Error") == TRUE) {
		set_idle(service);

		g_get_current_time(&service->modified);
		service_save(service);
	} else if (g_str_equal(name, "Passphrase") == TRUE) {
		if (service->immutable == TRUE)
			return __connman_error_not_supported(msg);

		g_free(service->passphrase);
		service->passphrase = NULL;

		passphrase_changed(service);

		service_save(service);
	} else
		return __connman_error_invalid_property(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static connman_bool_t is_ignore(struct connman_service *service)
{
	if (service->autoconnect == FALSE)
		return TRUE;

	if (service->roaming == TRUE)
		return TRUE;

	if (service->ignore == TRUE)
		return TRUE;

	if (service->state == CONNMAN_SERVICE_STATE_FAILURE)
		return TRUE;

	return FALSE;
}

void __connman_service_auto_connect(void)
{
	struct connman_service *service = NULL;
	GSequenceIter *iter;

	DBG("");

	if (__connman_session_mode() == TRUE) {
		DBG("Session mode enabled: auto connect disabled");
		return;
	}

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		service = g_sequence_get(iter);

		if (service->pending != NULL)
			return;

		if (is_connecting(service) == TRUE)
			return;

		if (service->favorite == FALSE)
			return;

		if (is_connected(service) == TRUE)
			return;

		if (is_ignore(service) == FALSE && service->state ==
						CONNMAN_SERVICE_STATE_IDLE)
			break;

		service = NULL;

		iter = g_sequence_iter_next(iter);
	}

	if (service != NULL) {
		service->userconnect = FALSE;
		__connman_service_connect(service);
	}
}

static void remove_timeout(struct connman_service *service)
{
	if (service->timeout > 0) {
		g_source_remove(service->timeout);
		service->timeout = 0;
	}
}

static void reply_pending(struct connman_service *service, int error)
{
	remove_timeout(service);

	if (service->pending != NULL) {
		if (error > 0) {
			DBusMessage *reply;

			reply = __connman_error_failed(service->pending,
								error);
			if (reply != NULL)
				g_dbus_send_message(connection, reply);
		} else {
			const char *sender;

			sender = dbus_message_get_interface(service->pending);

			DBG("sender %s", sender);

			if (g_strcmp0(sender, CONNMAN_MANAGER_INTERFACE) == 0)
				g_dbus_send_reply(connection, service->pending,
					DBUS_TYPE_OBJECT_PATH, &service->path,
							DBUS_TYPE_INVALID);
			else
				g_dbus_send_reply(connection, service->pending,
							DBUS_TYPE_INVALID);
		}

		dbus_message_unref(service->pending);
		service->pending = NULL;
	}
}

static gboolean connect_timeout(gpointer user_data)
{
	struct connman_service *service = user_data;
	connman_bool_t autoconnect = FALSE;

	DBG("service %p", service);

	service->timeout = 0;

	if (service->network != NULL)
		__connman_network_disconnect(service->network);

	__connman_ipconfig_disable(service->ipconfig_ipv4);
	__connman_ipconfig_disable(service->ipconfig_ipv6);

	__connman_stats_service_unregister(service);

	if (service->pending != NULL) {
		DBusMessage *reply;

		reply = __connman_error_operation_timeout(service->pending);
		if (reply != NULL)
			g_dbus_send_message(connection, reply);

		dbus_message_unref(service->pending);
		service->pending = NULL;
	} else
		autoconnect = TRUE;

	__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_FAILURE,
					CONNMAN_IPCONFIG_TYPE_IPV4);
	__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_FAILURE,
					CONNMAN_IPCONFIG_TYPE_IPV6);

	if (autoconnect == TRUE && service->userconnect == FALSE)
		__connman_service_auto_connect();

	return FALSE;
}

static void set_reconnect_state(struct connman_service *service,
						connman_bool_t reconnect)
{
	struct connman_device *device;

	if (service->network == NULL)
		return;

	device = connman_network_get_device(service->network);
	if (device == NULL)
		return;

	__connman_device_set_reconnect(device, reconnect);
}

static connman_bool_t get_reconnect_state(struct connman_service *service)
{
	struct connman_device *device;

	if (service->network == NULL)
		return FALSE;

	device = connman_network_get_device(service->network);
	if (device == NULL)
		return FALSE;

	return __connman_device_get_reconnect(device);
}

static DBusMessage *connect_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;
	GSequenceIter *iter;
	int err;

	DBG("service %p", service);

	if (service->pending != NULL)
		return __connman_error_in_progress(msg);

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		struct connman_service *temp = g_sequence_get(iter);

		if (service->type == temp->type && is_connecting(temp) == TRUE)
			return __connman_error_in_progress(msg);

		iter = g_sequence_iter_next(iter);
	}

	service->ignore = FALSE;

	service->userconnect = TRUE;

	service->pending = dbus_message_ref(msg);

	set_reconnect_state(service, FALSE);

	err = __connman_service_connect(service);
	if (err < 0) {
		if (service->pending == NULL)
			return NULL;

		if (err != -EINPROGRESS) {
			dbus_message_unref(service->pending);
			service->pending = NULL;

			return __connman_error_failed(msg, -err);
		}

		return NULL;
	}

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *disconnect_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;
	int err;

	DBG("service %p", service);

	reply_pending(service, ECONNABORTED);

	service->ignore = TRUE;

	set_reconnect_state(service, FALSE);

	err = __connman_service_disconnect(service);
	if (err < 0) {
		if (err != -EINPROGRESS)
			return __connman_error_failed(msg, -err);

		return NULL;
	}

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *remove_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;

	DBG("service %p", service);

	if (service->type == CONNMAN_SERVICE_TYPE_ETHERNET)
		return __connman_error_not_supported(msg);

	if (service->immutable == TRUE)
		return __connman_error_not_supported(msg);

	if (service->favorite == FALSE && service->state !=
						CONNMAN_SERVICE_STATE_FAILURE)
		return __connman_error_not_supported(msg);

	set_reconnect_state(service, FALSE);

	__connman_service_disconnect(service);

	g_free(service->passphrase);
	service->passphrase = NULL;

	passphrase_changed(service);

	set_idle(service);

	__connman_service_set_favorite(service, FALSE);
	service_save(service);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static gboolean check_suitable_state(enum connman_service_state a,
					enum connman_service_state b)
{
	/*
	 * Special check so that "ready" service can be moved before
	 * "online" one.
	 */
	if ((a == CONNMAN_SERVICE_STATE_ONLINE &&
			b == CONNMAN_SERVICE_STATE_READY) ||
		(b == CONNMAN_SERVICE_STATE_ONLINE &&
			a == CONNMAN_SERVICE_STATE_READY))
		return TRUE;

	return a == b;
}

static void downgrade_state(struct connman_service *service)
{
	if (service == NULL)
		return;

	DBG("service %p state4 %d state6 %d", service, service->state_ipv4,
						service->state_ipv6);

	if (service->state_ipv4 == CONNMAN_SERVICE_STATE_ONLINE)
		__connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_READY,
						CONNMAN_IPCONFIG_TYPE_IPV4);

	if (service->state_ipv6 == CONNMAN_SERVICE_STATE_ONLINE)
		__connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_READY,
						CONNMAN_IPCONFIG_TYPE_IPV6);
}

static void apply_relevant_default_downgrade(struct connman_service *service)
{
	struct connman_service *def_service;

	def_service = get_default();
	if (def_service == NULL)
		return;

	if (def_service == service &&
			def_service->state == CONNMAN_SERVICE_STATE_ONLINE)
		def_service->state = CONNMAN_SERVICE_STATE_READY;
}

static DBusMessage *move_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data,
								gboolean before)
{
	struct connman_service *service = user_data;
	struct connman_service *target;
	const char *path;
	GSequenceIter *src, *dst;
	enum connman_ipconfig_method target4, target6;
	enum connman_ipconfig_method service4, service6;

	DBG("service %p", service);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	if (service->favorite == FALSE)
		return __connman_error_not_supported(msg);

	target = find_service(path);
	if (target == NULL || target->favorite == FALSE || target == service ||
				target->type == CONNMAN_SERVICE_TYPE_VPN)
		return __connman_error_invalid_service(msg);

	target4 = __connman_ipconfig_get_method(target->ipconfig_ipv4);
	target6 = __connman_ipconfig_get_method(target->ipconfig_ipv6);
	service4 = __connman_ipconfig_get_method(service->ipconfig_ipv4);
	service6 = __connman_ipconfig_get_method(service->ipconfig_ipv6);

	DBG("target %s method %d/%d state %d/%d", target->identifier,
				target4, target6,
				target->state_ipv4, target->state_ipv6);

	DBG("service %s method %d/%d state %d/%d", service->identifier,
				service4, service6,
				service->state_ipv4, service->state_ipv6);

	/*
	 * If method is OFF, then we do not need to check the corresponding
	 * ipconfig state.
	 */
	if (target4 == CONNMAN_IPCONFIG_METHOD_OFF) {
		if (service6 != CONNMAN_IPCONFIG_METHOD_OFF) {
			if (check_suitable_state(target->state_ipv6,
						service->state_ipv6) == FALSE)
				return __connman_error_invalid_service(msg);
		}
	}

	if (target6 == CONNMAN_IPCONFIG_METHOD_OFF) {
		if (service4 != CONNMAN_IPCONFIG_METHOD_OFF) {
			if (check_suitable_state(target->state_ipv4,
						service->state_ipv4) == FALSE)
				return __connman_error_invalid_service(msg);
		}
	}

	if (service4 == CONNMAN_IPCONFIG_METHOD_OFF) {
		if (target6 != CONNMAN_IPCONFIG_METHOD_OFF) {
			if (check_suitable_state(target->state_ipv6,
						service->state_ipv6) == FALSE)
				return __connman_error_invalid_service(msg);
		}
	}

	if (service6 == CONNMAN_IPCONFIG_METHOD_OFF) {
		if (target4 != CONNMAN_IPCONFIG_METHOD_OFF) {
			if (check_suitable_state(target->state_ipv4,
						service->state_ipv4) == FALSE)
				return __connman_error_invalid_service(msg);
		}
	}

	g_get_current_time(&service->modified);
	service_save(service);

	src = g_hash_table_lookup(service_hash, service->identifier);
	dst = g_hash_table_lookup(service_hash, target->identifier);

	/*
	 * If the service which goes down is the default service and is
	 * online, we downgrade directly its state to ready so:
	 * the service which goes up, needs to recompute its state which
	 * is triggered via downgrading it - if relevant - to state ready.
	 */
	if (before == TRUE) {
		apply_relevant_default_downgrade(target);
		g_sequence_move(src, dst);
		downgrade_state(service);
	} else {
		apply_relevant_default_downgrade(service);
		g_sequence_move(dst, src);
		downgrade_state(target);
	}

	services_changed(FALSE);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *move_before(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return move_service(conn, msg, user_data, TRUE);
}

static DBusMessage *move_after(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return move_service(conn, msg, user_data, FALSE);
}

static DBusMessage *reset_counters(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_service *service = user_data;

	reset_stats(service);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static GDBusMethodTable service_methods[] = {
	{ "GetProperties", "",   "a{sv}", get_properties     },
	{ "SetProperty",   "sv", "",      set_property       },
	{ "ClearProperty", "s",  "",      clear_property     },
	{ "Connect",       "",   "",      connect_service,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Disconnect",    "",   "",      disconnect_service },
	{ "Remove",        "",   "",      remove_service     },
	{ "MoveBefore",    "o",  "",      move_before        },
	{ "MoveAfter",     "o",  "",      move_after         },
	{ "ResetCounters", "",   "",      reset_counters     },
	{ },
};

static GDBusSignalTable service_signals[] = {
	{ "PropertyChanged", "sv" },
	{ },
};

static void service_free(gpointer user_data)
{
	struct connman_service *service = user_data;
	char *path = service->path;

	DBG("service %p", service);

	reply_pending(service, ENOENT);

	g_hash_table_remove(service_hash, service->identifier);

	__connman_notifier_service_remove(service);

	stats_stop(service);

	service->path = NULL;

	if (path != NULL) {
		services_changed(FALSE);

		g_dbus_unregister_interface(connection, path,
						CONNMAN_SERVICE_INTERFACE);
		g_free(path);
	}

	g_hash_table_destroy(service->counter_table);

	if (service->network != NULL) {
		if (service->network_created == TRUE)
			connman_network_unref(service->network);
	}

	if (service->provider != NULL)
		connman_provider_unref(service->provider);

	if (service->ipconfig_ipv4 != NULL) {
		connman_ipconfig_set_ops(service->ipconfig_ipv4, NULL);
		connman_ipconfig_set_data(service->ipconfig_ipv4, NULL);
		connman_ipconfig_unref(service->ipconfig_ipv4);
		service->ipconfig_ipv4 = NULL;
	}

	if (service->ipconfig_ipv6 != NULL) {
		connman_ipconfig_set_ops(service->ipconfig_ipv6, NULL);
		connman_ipconfig_set_data(service->ipconfig_ipv6, NULL);
		connman_ipconfig_unref(service->ipconfig_ipv6);
		service->ipconfig_ipv6 = NULL;
	}

	g_strfreev(service->nameservers);
	g_strfreev(service->nameservers_config);
	g_strfreev(service->nameservers_auto);
	g_strfreev(service->domains);
	g_strfreev(service->proxies);
	g_strfreev(service->excludes);

	g_free(service->domainname);
	g_free(service->pac);
	g_free(service->name);
	g_free(service->passphrase);
	g_free(service->agent_passphrase);
	g_free(service->identifier);
	g_free(service->eap);
	g_free(service->identity);
	g_free(service->agent_identity);
	g_free(service->ca_cert_file);
	g_free(service->client_cert_file);
	g_free(service->private_key_file);
	g_free(service->private_key_passphrase);
	g_free(service->phase2);

	if (service->stats.timer != NULL)
		g_timer_destroy(service->stats.timer);
	if (service->stats_roaming.timer != NULL)
		g_timer_destroy(service->stats_roaming.timer);

	g_free(service);
}

/**
 * __connman_service_put:
 * @service: service structure
 *
 * Release service if no longer needed
 */
void __connman_service_put(struct connman_service *service)
{
	GSequenceIter *iter;

	DBG("service %p", service);

	if (__sync_fetch_and_sub(&service->refcount, 1) != 1)
		return;

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter != NULL) {
		reply_pending(service, ECONNABORTED);

		__connman_service_disconnect(service);

		g_sequence_remove(iter);
	} else {
		service_free(service);
	}
}

static void stats_init(struct connman_service *service)
{
	/* home */
	service->stats.valid = FALSE;
	service->stats.enabled = FALSE;
	service->stats.timer = g_timer_new();

	/* roaming */
	service->stats_roaming.valid = FALSE;
	service->stats_roaming.enabled = FALSE;
	service->stats_roaming.timer = g_timer_new();
}

static void service_initialize(struct connman_service *service)
{
	DBG("service %p", service);

	service->refcount = 1;
	service->session_usage_count = 0;

	service->network_created = FALSE;

	service->type     = CONNMAN_SERVICE_TYPE_UNKNOWN;
	service->security = CONNMAN_SERVICE_SECURITY_UNKNOWN;

	service->state = CONNMAN_SERVICE_STATE_UNKNOWN;
	service->state_ipv4 = CONNMAN_SERVICE_STATE_UNKNOWN;
	service->state_ipv6 = CONNMAN_SERVICE_STATE_UNKNOWN;

	service->favorite  = FALSE;
	service->immutable = FALSE;
	service->hidden = FALSE;

	service->ignore = FALSE;

	service->userconnect = FALSE;

	service->order = 0;

	stats_init(service);

	service->provider = NULL;

	service->wps = FALSE;
}

/**
 * connman_service_create:
 *
 * Allocate a new service.
 *
 * Returns: a newly-allocated #connman_service structure
 */
struct connman_service *connman_service_create(void)
{
	GSList *list;
	struct connman_stats_counter *counters;
	const char *counter;

	struct connman_service *service;

	service = g_try_new0(struct connman_service, 1);
	if (service == NULL)
		return NULL;

	DBG("service %p", service);

	service->counter_table = g_hash_table_new_full(g_str_hash,
						g_str_equal, NULL, g_free);

	for (list = counter_list; list; list = list->next) {
		counter = list->data;

		counters = g_try_new0(struct connman_stats_counter, 1);
		if (counters == NULL) {
			g_hash_table_destroy(service->counter_table);
			g_free(service);
			return NULL;
		}

		counters->append_all = TRUE;

		g_hash_table_replace(service->counter_table, (gpointer)counter,
				counters);
	}

	service_initialize(service);

	return service;
}

/**
 * connman_service_ref:
 * @service: service structure
 *
 * Increase reference counter of service
 */
struct connman_service *connman_service_ref(struct connman_service *service)
{
	DBG("%p", service);

	__sync_fetch_and_add(&service->refcount, 1);

	return service;
}

/**
 * connman_service_unref:
 * @service: service structure
 *
 * Decrease reference counter of service
 */
void connman_service_unref(struct connman_service *service)
{
	__connman_service_put(service);
}

static gint service_compare(gconstpointer a, gconstpointer b,
							gpointer user_data)
{
	struct connman_service *service_a = (void *) a;
	struct connman_service *service_b = (void *) b;
	enum connman_service_state state_a, state_b;

	state_a = service_a->state;
	state_b = service_b->state;

	if (state_a != state_b) {
		gboolean a_connected = is_connected(service_a);
		gboolean b_connected = is_connected(service_b);

		if (a_connected == TRUE && b_connected == TRUE) {
			/* We prefer online over ready state */
			if (state_a == CONNMAN_SERVICE_STATE_ONLINE)
				return -1;

			if (state_b == CONNMAN_SERVICE_STATE_ONLINE)
				return 1;
		}

		if (a_connected == TRUE)
			return -1;
		if (b_connected == TRUE)
			return 1;

		if (is_connecting(service_a) == TRUE)
			return -1;
		if (is_connecting(service_b) == TRUE)
			return 1;
	}

	if (service_a->order > service_b->order)
		return -1;

	if (service_a->order < service_b->order)
		return 1;

	if (service_a->favorite == TRUE && service_b->favorite == FALSE)
		return -1;

	if (service_a->favorite == FALSE && service_b->favorite == TRUE)
		return 1;

	if (service_a->type != service_b->type) {
		switch (service_a->type) {
		case CONNMAN_SERVICE_TYPE_UNKNOWN:
		case CONNMAN_SERVICE_TYPE_SYSTEM:
		case CONNMAN_SERVICE_TYPE_ETHERNET:
		case CONNMAN_SERVICE_TYPE_GPS:
		case CONNMAN_SERVICE_TYPE_VPN:
		case CONNMAN_SERVICE_TYPE_GADGET:
			break;
		case CONNMAN_SERVICE_TYPE_WIFI:
			return 1;
		case CONNMAN_SERVICE_TYPE_WIMAX:
		case CONNMAN_SERVICE_TYPE_BLUETOOTH:
		case CONNMAN_SERVICE_TYPE_CELLULAR:
			return -1;
		}
	}

	return (gint) service_b->strength - (gint) service_a->strength;
}

/**
 * connman_service_get_type:
 * @service: service structure
 *
 * Get the type of service
 */
enum connman_service_type connman_service_get_type(struct connman_service *service)
{
	if (service == NULL)
		return CONNMAN_SERVICE_TYPE_UNKNOWN;

	return service->type;
}

/**
 * connman_service_get_interface:
 * @service: service structure
 *
 * Get network interface of service
 */
char *connman_service_get_interface(struct connman_service *service)
{
	int index;

	if (service == NULL)
		return NULL;

	if (service->type == CONNMAN_SERVICE_TYPE_VPN) {
		if (service->ipconfig_ipv4)
			index = connman_ipconfig_get_index(
						service->ipconfig_ipv4);
		else if (service->ipconfig_ipv6)
			index = connman_ipconfig_get_index(
						service->ipconfig_ipv6);
		else
			return NULL;

		return connman_inet_ifname(index);
	}

	if (service->network == NULL)
		return NULL;

	index = connman_network_get_index(service->network);

	return connman_inet_ifname(index);
}

/**
 * connman_service_get_network:
 * @service: service structure
 *
 * Get the service network
 */
struct connman_network *
__connman_service_get_network(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	return service->network;
}

struct connman_ipconfig *
__connman_service_get_ip4config(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	return service->ipconfig_ipv4;
}

struct connman_ipconfig *
__connman_service_get_ip6config(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	return service->ipconfig_ipv6;
}

struct connman_ipconfig *
__connman_service_get_ipconfig(struct connman_service *service, int family)
{
	if (family == AF_INET)
		return __connman_service_get_ip4config(service);
	else if (family == AF_INET6)
		return __connman_service_get_ip6config(service);
	else
		return NULL;

}

enum connman_service_security __connman_service_get_security(struct connman_service *service)
{
	if (service == NULL)
		return CONNMAN_SERVICE_SECURITY_UNKNOWN;

	return service->security;
}

const char *__connman_service_get_phase2(struct connman_service *service)
{
	if (service == NULL)
		return NULL;

	return service->phase2;
}

connman_bool_t __connman_service_wps_enabled(struct connman_service *service)
{
	if (service == NULL)
		return FALSE;

	return service->wps;
}

/**
 * __connman_service_set_favorite:
 * @service: service structure
 * @favorite: favorite value
 *
 * Change the favorite setting of service
 */
int __connman_service_set_favorite(struct connman_service *service,
						connman_bool_t favorite)
{
	GSequenceIter *iter;

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter == NULL)
		return -ENOENT;

	if (service->favorite == favorite)
		return -EALREADY;

	service->favorite = favorite;
	service->order = __connman_service_get_order(service);

	favorite_changed(service);

	g_sequence_sort_changed(iter, service_compare, NULL);

	services_changed(FALSE);

	return 0;
}

int __connman_service_set_immutable(struct connman_service *service,
						connman_bool_t immutable)
{
	service->immutable = immutable;

	immutable_changed(service);

	return 0;
}

void __connman_service_set_string(struct connman_service *service,
				  const char *key, const char *value)
{
	if (g_str_equal(key, "EAP") == TRUE) {
		g_free(service->eap);
		service->eap = g_strdup(value);
	} else if (g_str_equal(key, "Identity") == TRUE) {
		g_free(service->identity);
		service->identity = g_strdup(value);
	} else if (g_str_equal(key, "CACertFile") == TRUE) {
		g_free(service->ca_cert_file);
		service->ca_cert_file = g_strdup(value);
	} else if (g_str_equal(key, "ClientCertFile") == TRUE) {
		g_free(service->client_cert_file);
		service->client_cert_file = g_strdup(value);
	} else if (g_str_equal(key, "PrivateKeyFile") == TRUE) {
		g_free(service->private_key_file);
		service->private_key_file = g_strdup(value);
	} else if (g_str_equal(key, "PrivateKeyPassphrase") == TRUE) {
		g_free(service->private_key_passphrase);
		service->private_key_passphrase = g_strdup(value);
	} else if (g_str_equal(key, "Phase2") == TRUE) {
		g_free(service->phase2);
		service->phase2 = g_strdup(value);
	} else if (g_str_equal(key, "Passphrase") == TRUE) {
		g_free(service->passphrase);
		service->passphrase = g_strdup(value);
	}
}

static void service_complete(struct connman_service *service)
{
	reply_pending(service, EIO);

	if (service->userconnect == FALSE)
		__connman_service_auto_connect();

	g_get_current_time(&service->modified);
	service_save(service);
}

static void report_error_cb(struct connman_service *service,
			gboolean retry, void *user_data)
{
	if (retry == TRUE)
		__connman_service_connect(service);
	else {
		service_complete(service);
		services_changed(FALSE);
		__connman_device_request_scan(CONNMAN_DEVICE_TYPE_UNKNOWN);
	}
}

static void request_input_cb (struct connman_service *service,
			const char *identity, const char *passphrase,
			void *user_data)
{
	DBG ("RequestInput return, %p", service);

	if (identity == NULL && passphrase == NULL && service->wps == FALSE) {
		service_complete(service);
		services_changed(FALSE);
		__connman_device_request_scan(CONNMAN_DEVICE_TYPE_UNKNOWN);
		return;
	}

	if (identity != NULL)
		__connman_service_set_agent_identity(service, identity);

	if (passphrase != NULL) {
		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_PSK:
			__connman_service_set_passphrase(service, passphrase);
			break;
		case CONNMAN_SERVICE_SECURITY_8021X:
			__connman_service_set_agent_passphrase(service,
							passphrase);
			break;
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			DBG("service security '%s' not handled",
				security2string(service->security));
			break;
		}
	}

	__connman_service_connect(service);

	/* Never cache agent provided credentials */
	__connman_service_set_agent_identity(service, NULL);
	__connman_service_set_agent_passphrase(service, NULL);
}

static void downgrade_connected_services(void)
{
	struct connman_service *up_service;
	GSequenceIter *iter;

	iter = g_sequence_get_begin_iter(service_list);
	while (g_sequence_iter_is_end(iter) == FALSE) {
		up_service = g_sequence_get(iter);

		if (is_connected(up_service) == FALSE) {
			iter = g_sequence_iter_next(iter);
			continue;
		}

		if (up_service->state == CONNMAN_SERVICE_STATE_ONLINE)
			return;

		downgrade_state(up_service);

		iter = g_sequence_iter_next(iter);
	}
}

static int service_indicate_state(struct connman_service *service)
{
	enum connman_service_state old_state, new_state;
	struct connman_service *def_service;
	GSequenceIter *iter;

	if (service == NULL)
		return -EINVAL;

	old_state = service->state;
	new_state = combine_state(service->state_ipv4, service->state_ipv6);

	DBG("service %p old %s - new %s/%s => %s",
					service,
					state2string(old_state),
					state2string(service->state_ipv4),
					state2string(service->state_ipv6),
					state2string(new_state));

	if (old_state == new_state)
		return -EALREADY;

	def_service = get_default();

	if (new_state == CONNMAN_SERVICE_STATE_ONLINE) {
		if (def_service != NULL && def_service != service &&
			def_service->state == CONNMAN_SERVICE_STATE_ONLINE)
			return -EALREADY;
	}

	service->state = new_state;
	state_changed(service);

	if (new_state == CONNMAN_SERVICE_STATE_IDLE &&
			old_state != CONNMAN_SERVICE_STATE_DISCONNECT) {
		reply_pending(service, ECONNABORTED);

		__connman_service_disconnect(service);
	}

	if (new_state == CONNMAN_SERVICE_STATE_CONFIGURATION) {
		if (__connman_stats_service_register(service) == 0) {
			__connman_stats_get(service, FALSE,
						&service->stats.data);
			__connman_stats_get(service, TRUE,
						&service->stats_roaming.data);
		}
	}

	if (new_state == CONNMAN_SERVICE_STATE_ONLINE) {
		if (service->login_required == TRUE) {
			service->login_required = FALSE;
			login_changed(service);
		}

		connman_timeserver_sync();
	}

	if (new_state == CONNMAN_SERVICE_STATE_IDLE) {
		connman_bool_t reconnect;

		reconnect = get_reconnect_state(service);
		if (reconnect == TRUE)
			__connman_service_auto_connect();

		__connman_device_request_scan(CONNMAN_DEVICE_TYPE_UNKNOWN);
	}

	if (new_state == CONNMAN_SERVICE_STATE_READY) {
		enum connman_ipconfig_method method;

		set_reconnect_state(service, TRUE);

		__connman_service_set_favorite(service, TRUE);

		reply_pending(service, 0);

		service->userconnect = FALSE;

		g_get_current_time(&service->modified);
		service_save(service);

		update_nameservers(service);
		dns_changed(service);
		domain_changed(service);

		__connman_notifier_connect(service->type);

		if (service->type == CONNMAN_SERVICE_TYPE_WIFI &&
			connman_network_get_bool(service->network,
						"WiFi.UseWPS") == TRUE) {
			const char *pass;

			pass = connman_network_get_string(service->network,
							"WiFi.Passphrase");

			__connman_service_set_passphrase(service, pass);

			connman_network_set_bool(service->network,
							"WiFi.UseWPS", FALSE);
		}

		default_changed();

		method = __connman_ipconfig_get_method(service->ipconfig_ipv6);
		if (method == CONNMAN_IPCONFIG_METHOD_OFF)
			__connman_ipconfig_disable_ipv6(
						service->ipconfig_ipv6);

	} else if (new_state == CONNMAN_SERVICE_STATE_DISCONNECT) {
		def_service = get_default();

		if (__connman_notifier_count_connected() == 0 &&
			def_service != NULL &&
				def_service->provider != NULL)
			__connman_provider_disconnect(def_service->provider);

		default_changed();

		__connman_wispr_stop(service);

		__connman_wpad_stop(service);

		update_nameservers(service);
		dns_changed(service);
		domain_changed(service);

		__connman_notifier_disconnect(service->type);

		/*
		 * Previous services which are connected and which states
		 * are set to online should reset relevantly ipconfig_state
		 * to ready so wispr/portal will be rerun on those
		 */
		downgrade_connected_services();
	}

	if (new_state == CONNMAN_SERVICE_STATE_FAILURE) {
		if (service->userconnect == TRUE &&
			__connman_agent_report_error(service,
					error2string(service->error),
					report_error_cb, NULL) == -EIO)
			return 0;
		service_complete(service);

		__connman_device_request_scan(CONNMAN_DEVICE_TYPE_UNKNOWN);
	} else
		service->error = CONNMAN_SERVICE_ERROR_UNKNOWN;

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter != NULL)
		g_sequence_sort_changed(iter, service_compare, NULL);

	services_changed(FALSE);

	if (new_state == CONNMAN_SERVICE_STATE_ONLINE)
		default_changed();

	return 0;
}

int __connman_service_indicate_error(struct connman_service *service,
					enum connman_service_error error)
{
	DBG("service %p error %d", service, error);

	if (service == NULL)
		return -EINVAL;

	service->error = error;

	if (service->error == CONNMAN_SERVICE_ERROR_INVALID_KEY)
		__connman_service_set_passphrase(service, NULL);

	__connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_FAILURE,
						CONNMAN_IPCONFIG_TYPE_IPV4);
	__connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_FAILURE,
						CONNMAN_IPCONFIG_TYPE_IPV6);
	return 0;
}

int __connman_service_clear_error(struct connman_service *service)
{
	DBG("service %p", service);

	if (service == NULL)
		return -EINVAL;

	if (service->state != CONNMAN_SERVICE_STATE_FAILURE)
		return -EINVAL;

	service->state_ipv4 = service->state_ipv6 =
						CONNMAN_SERVICE_STATE_UNKNOWN;
	service->error = CONNMAN_SERVICE_ERROR_UNKNOWN;;

	if (service->favorite == TRUE)
		set_reconnect_state(service, TRUE);

	__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_IDLE,
					CONNMAN_IPCONFIG_TYPE_IPV6);

	/*
	 * Toggling the IPv6 state to IDLE could trigger the auto connect
	 * machinery and consequently the IPv4 state.
	 */
	if (service->state_ipv4 != CONNMAN_SERVICE_STATE_UNKNOWN &&
			service->state_ipv4 != CONNMAN_SERVICE_STATE_FAILURE)
		return 0;

	return __connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_IDLE,
						CONNMAN_IPCONFIG_TYPE_IPV4);
}

int __connman_service_indicate_default(struct connman_service *service)
{
	DBG("service %p", service);

	default_changed();

	return 0;
}

enum connman_service_state __connman_service_ipconfig_get_state(
					struct connman_service *service,
					enum connman_ipconfig_type type)
{
	if (service == NULL)
		return CONNMAN_SERVICE_STATE_UNKNOWN;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		return service->state_ipv4;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV6)
		return service->state_ipv6;

	return CONNMAN_SERVICE_STATE_UNKNOWN;
}

static void check_proxy_setup(struct connman_service *service)
{
	/*
	 * We start WPAD if we haven't got a PAC URL from DHCP and
	 * if our proxy manual configuration is either empty or set
	 * to AUTO with an empty URL.
	 */

	if (service->proxy != CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN)
		goto done;

	if (service->proxy_config != CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN &&
		(service->proxy_config != CONNMAN_SERVICE_PROXY_METHOD_AUTO ||
			service->pac != NULL))
		goto done;

	if (__connman_wpad_start(service) < 0) {
		service->proxy = CONNMAN_SERVICE_PROXY_METHOD_DIRECT;
		__connman_notifier_proxy_changed(service);
		goto done;
	}

	return;

done:
	__connman_wispr_start(service, CONNMAN_IPCONFIG_TYPE_IPV4);
}

/*
 * How many networks are connected at the same time. If more than 1,
 * then set the rp_filter setting properly (loose mode routing) so that network
 * connectivity works ok. This is only done for IPv4 networks as IPv6
 * does not have rp_filter knob.
 */
static int connected_networks_count;
static int original_rp_filter;

static void service_rp_filter(struct connman_service *service,
				gboolean connected)
{
	enum connman_ipconfig_method method;

	method = __connman_ipconfig_get_method(service->ipconfig_ipv4);

	switch (method) {
	case CONNMAN_IPCONFIG_METHOD_UNKNOWN:
	case CONNMAN_IPCONFIG_METHOD_OFF:
	case CONNMAN_IPCONFIG_METHOD_AUTO:
		return;
	case CONNMAN_IPCONFIG_METHOD_FIXED:
	case CONNMAN_IPCONFIG_METHOD_MANUAL:
	case CONNMAN_IPCONFIG_METHOD_DHCP:
		break;
	}

	if (connected == TRUE) {
		if (connected_networks_count == 1) {
			int filter_value;
			filter_value = __connman_ipconfig_set_rp_filter();
			if (filter_value < 0)
				return;

			original_rp_filter = filter_value;
		}
		connected_networks_count++;

	} else {
		if (connected_networks_count == 2)
			__connman_ipconfig_unset_rp_filter(original_rp_filter);

		connected_networks_count--;
		if (connected_networks_count < 0)
			connected_networks_count = 0;
	}

	DBG("%s %s ipconfig %p method %d count %d filter %d",
		connected ? "connected" : "disconnected", service->identifier,
		service->ipconfig_ipv4, method,
		connected_networks_count, original_rp_filter);
}

int __connman_service_ipconfig_indicate_state(struct connman_service *service,
					enum connman_service_state new_state,
					enum connman_ipconfig_type type)
{
	struct connman_ipconfig *ipconfig = NULL;
	enum connman_service_state old_state;
	int ret;

	if (service == NULL)
		return -EINVAL;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		old_state = service->state_ipv4;
		ipconfig = service->ipconfig_ipv4;
	} else if (type == CONNMAN_IPCONFIG_TYPE_IPV6) {
		old_state = service->state_ipv6;
		ipconfig = service->ipconfig_ipv6;
	}

	if (ipconfig == NULL)
		return -EINVAL;

	/* Any change? */
	if (old_state == new_state)
		return -EALREADY;

	DBG("service %p (%s) state %d (%s) type %d (%s)",
		service, service ? service->identifier : NULL,
		new_state, state2string(new_state),
		type, __connman_ipconfig_type2string(type));

	switch (new_state) {
	case CONNMAN_SERVICE_STATE_UNKNOWN:
	case CONNMAN_SERVICE_STATE_IDLE:
		if (service->state == CONNMAN_SERVICE_STATE_FAILURE)
			return -EINVAL;
		break;
	case CONNMAN_SERVICE_STATE_ASSOCIATION:
		break;
	case CONNMAN_SERVICE_STATE_CONFIGURATION:
		__connman_ipconfig_enable(ipconfig);
		break;
	case CONNMAN_SERVICE_STATE_READY:
		update_nameservers(service);

		if (type == CONNMAN_IPCONFIG_TYPE_IPV4) {
			check_proxy_setup(service);
			service_rp_filter(service, TRUE);
		} else
			__connman_wispr_start(service, type);
		break;
	case CONNMAN_SERVICE_STATE_ONLINE:
		break;
	case CONNMAN_SERVICE_STATE_DISCONNECT:
		if (service->state == CONNMAN_SERVICE_STATE_IDLE)
			return -EINVAL;

		if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
			service_rp_filter(service, FALSE);

		break;
	case CONNMAN_SERVICE_STATE_FAILURE:
		break;
	}

	/* We keep that state */
	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		service->state_ipv4 = new_state;
	else if (type == CONNMAN_IPCONFIG_TYPE_IPV6)
		service->state_ipv6 = new_state;

	ret = service_indicate_state(service);

	/*
	 * If the ipconfig method is OFF, then we set the state to IDLE
	 * so that it will not affect the combined state in the future.
	 */
	if (type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		enum connman_ipconfig_method method;
		method = __connman_ipconfig_get_method(service->ipconfig_ipv4);
		if (method == CONNMAN_IPCONFIG_METHOD_OFF ||
				method == CONNMAN_IPCONFIG_METHOD_UNKNOWN) {
			service->state_ipv4 = CONNMAN_SERVICE_STATE_IDLE;
			ret = service_indicate_state(service);
		}

	} else if (type == CONNMAN_IPCONFIG_TYPE_IPV6) {
		enum connman_ipconfig_method method;
		method = __connman_ipconfig_get_method(service->ipconfig_ipv6);
		if (method == CONNMAN_IPCONFIG_METHOD_OFF ||
				method == CONNMAN_IPCONFIG_METHOD_UNKNOWN) {
			service->state_ipv6 = CONNMAN_SERVICE_STATE_IDLE;
			ret = service_indicate_state(service);
		}
	}

	return ret;
}

int __connman_service_request_login(struct connman_service *service)
{
	DBG("service %p", service);

	if (service == NULL)
		return -EINVAL;

	service->login_required = TRUE;
	login_changed(service);

	return 0;
}

static connman_bool_t prepare_network(struct connman_service *service)
{
	enum connman_network_type type;
	unsigned int ssid_len;

	type = connman_network_get_type(service->network);

	switch (type) {
	case CONNMAN_NETWORK_TYPE_UNKNOWN:
	case CONNMAN_NETWORK_TYPE_VENDOR:
		return FALSE;
	case CONNMAN_NETWORK_TYPE_WIFI:
		if (connman_network_get_blob(service->network, "WiFi.SSID",
							&ssid_len) == NULL)
			return FALSE;

		if (service->passphrase != NULL)
			connman_network_set_string(service->network,
				"WiFi.Passphrase", service->passphrase);
		break;
	case CONNMAN_NETWORK_TYPE_ETHERNET:
	case CONNMAN_NETWORK_TYPE_WIMAX:
	case CONNMAN_NETWORK_TYPE_BLUETOOTH_PAN:
	case CONNMAN_NETWORK_TYPE_BLUETOOTH_DUN:
	case CONNMAN_NETWORK_TYPE_CELLULAR:
		break;
	}

	return TRUE;
}

static void prepare_8021x(struct connman_service *service)
{
	if (service->eap != NULL)
		connman_network_set_string(service->network, "WiFi.EAP",
								service->eap);

	if (service->identity != NULL)
		connman_network_set_string(service->network, "WiFi.Identity",
							service->identity);

	if (service->ca_cert_file != NULL)
		connman_network_set_string(service->network, "WiFi.CACertFile",
							service->ca_cert_file);

	if (service->client_cert_file != NULL)
		connman_network_set_string(service->network,
						"WiFi.ClientCertFile",
						service->client_cert_file);

	if (service->private_key_file != NULL)
		connman_network_set_string(service->network,
						"WiFi.PrivateKeyFile",
						service->private_key_file);

	if (service->private_key_passphrase != NULL)
		connman_network_set_string(service->network,
					"WiFi.PrivateKeyPassphrase",
					service->private_key_passphrase);

	if (service->phase2 != NULL)
		connman_network_set_string(service->network, "WiFi.Phase2",
							service->phase2);
}

static int service_connect(struct connman_service *service)
{
	int err;

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return -EINVAL;
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
	case CONNMAN_SERVICE_TYPE_VPN:
		break;
	case CONNMAN_SERVICE_TYPE_WIFI:
		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
			break;
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_PSK:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			if (service->passphrase == NULL) {
				if (service->network == NULL)
					return -EOPNOTSUPP;

				if (service->wps == FALSE ||
					connman_network_get_bool(
							service->network,
							"WiFi.UseWPS") == FALSE)
					return -ENOKEY;
			}
			break;
		case CONNMAN_SERVICE_SECURITY_8021X:
			if (service->eap == NULL)
				return -EINVAL;

			/*
			 * never request credentials if using EAP-TLS
			 * (EAP-TLS networks need to be fully provisioned)
			 */
			if (g_str_equal(service->eap, "tls") == TRUE)
				break;

			/*
			 * Return -ENOKEY if either identity or passphrase is
			 * missing. Agent provided credentials can be used as
			 * fallback if needed.
			 */
			if ((service->identity == NULL &&
					service->agent_identity == NULL) ||
					(service->passphrase == NULL &&
					service->agent_passphrase == NULL))
				return -ENOKEY;

			break;
		}
		break;
	}

	if (service->network != NULL) {
		if (prepare_network(service) == FALSE)
			return -EINVAL;

		switch (service->security) {
		case CONNMAN_SERVICE_SECURITY_UNKNOWN:
		case CONNMAN_SERVICE_SECURITY_NONE:
		case CONNMAN_SERVICE_SECURITY_WEP:
		case CONNMAN_SERVICE_SECURITY_PSK:
		case CONNMAN_SERVICE_SECURITY_WPA:
		case CONNMAN_SERVICE_SECURITY_RSN:
			break;
		case CONNMAN_SERVICE_SECURITY_8021X:
			prepare_8021x(service);
			break;
		}

		if (__connman_stats_service_register(service) == 0) {
			__connman_stats_get(service, FALSE,
						&service->stats.data);
			__connman_stats_get(service, TRUE,
						&service->stats_roaming.data);
		}

		if (service->ipconfig_ipv4)
			__connman_ipconfig_enable(service->ipconfig_ipv4);
		if (service->ipconfig_ipv6)
			__connman_ipconfig_enable(service->ipconfig_ipv6);

		err = __connman_network_connect(service->network);
	} else if (service->type == CONNMAN_SERVICE_TYPE_VPN &&
					service->provider != NULL)
		err = __connman_provider_connect(service->provider);
	else
		return -EOPNOTSUPP;

	if (err < 0) {
		if (err != -EINPROGRESS) {
			__connman_ipconfig_disable(service->ipconfig_ipv4);
			__connman_ipconfig_disable(service->ipconfig_ipv6);
			__connman_stats_service_unregister(service);
		}
	}

	return err;
}


int __connman_service_connect(struct connman_service *service)
{
	int err;

	DBG("service %p state %s", service, state2string(service->state));

	if (is_connected(service) == TRUE)
		return -EISCONN;

	if (is_connecting(service) == TRUE)
		return -EALREADY;

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return -EINVAL;
	default:
		err = service_connect(service);
	}

	if (err >= 0)
		return 0;

	if (err == -EINPROGRESS) {
		if (service->timeout == 0)
			service->timeout = g_timeout_add_seconds(
				CONNECT_TIMEOUT, connect_timeout, service);

		return -EINPROGRESS;
	}

	__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_FAILURE,
					CONNMAN_IPCONFIG_TYPE_IPV4);
	__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_FAILURE,
					CONNMAN_IPCONFIG_TYPE_IPV6);

	if (service->network != NULL)
		__connman_network_disconnect(service->network);
	else if (service->type == CONNMAN_SERVICE_TYPE_VPN &&
				service->provider != NULL)
			__connman_provider_disconnect(service->provider);

	if (service->userconnect == TRUE) {
		if (err == -ENOKEY) {
			if (__connman_agent_request_passphrase_input(service,
							request_input_cb,
							NULL) == -EIO)
				return -EINPROGRESS;
		}
		reply_pending(service, err);
	}

	return err;
}

int __connman_service_disconnect(struct connman_service *service)
{
	int err;

	DBG("service %p", service);

	if (service->network != NULL) {
		err = __connman_network_disconnect(service->network);
	} else if (service->type == CONNMAN_SERVICE_TYPE_VPN &&
					service->provider != NULL)
		err = __connman_provider_disconnect(service->provider);
	else
		return -EOPNOTSUPP;

	if (err < 0 && err != -EINPROGRESS)
		return err;

	__connman_6to4_remove(service->ipconfig_ipv4);

	if (service->ipconfig_ipv4)
		__connman_ipconfig_set_proxy_autoconfig(service->ipconfig_ipv4,
							NULL);
	else
		__connman_ipconfig_set_proxy_autoconfig(service->ipconfig_ipv6,
							NULL);

	__connman_ipconfig_address_remove(service->ipconfig_ipv4);
	__connman_ipconfig_address_remove(service->ipconfig_ipv6);

	__connman_ipconfig_disable(service->ipconfig_ipv4);
	__connman_ipconfig_disable(service->ipconfig_ipv6);

	__connman_stats_service_unregister(service);

	return err;
}

int __connman_service_disconnect_all(void)
{
	GSequenceIter *iter;
	GSList *services = NULL, *list;

	DBG("");

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		struct connman_service *service = g_sequence_get(iter);

		services = g_slist_prepend(services, service);

		iter = g_sequence_iter_next(iter);
	}

	for (list = services; list != NULL; list = list->next) {
		struct connman_service *service = list->data;

		service->ignore = TRUE;

		set_reconnect_state(service, FALSE);

		__connman_service_disconnect(service);
	}

	g_slist_free(list);

	return 0;

}

/**
 * __connman_service_lookup:
 * @pattern: search pattern
 * @path: return object path
 *
 * Look up a service path from a search pattern
 */
int __connman_service_lookup(const char *pattern, const char **path)
{
	GHashTableIter iter;
	gpointer key, value;
	struct connman_device *device;
	const char *ifname;

	g_hash_table_iter_init(&iter, service_hash);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GSequenceIter *iter = value;
		struct connman_service *service = g_sequence_get(iter);

		if (g_strcmp0(service->identifier, pattern) == 0 ||
				g_strcmp0(service->name, pattern) == 0) {
			*path = (const char *) service->path;
			return 0;
		}

		if (service->network == NULL)
			continue;

		device = connman_network_get_device(service->network);
		if (device == NULL)
			continue;

		ifname = connman_device_get_string(device, "Interface");
		if (ifname != NULL && g_strcmp0(ifname, pattern) == 0) {
			*path = (const char *) service->path;
			return 0;
		}

	}

	return -ENXIO;
}

/**
 * lookup_by_identifier:
 * @identifier: service identifier
 *
 * Look up a service by identifier (reference count will not be increased)
 */
static struct connman_service *lookup_by_identifier(const char *identifier)
{
	GSequenceIter *iter;

	iter = g_hash_table_lookup(service_hash, identifier);
	if (iter != NULL)
		return g_sequence_get(iter);

	return NULL;
}

static struct connman_network *create_hidden_wifi(struct connman_device *device,
		const char *ssid, const char *mode, const char *security,
		const char *group)
{
	struct connman_network *network;
	char *name;
	int index;
	unsigned int i, ssid_len;

	ssid_len = strlen(ssid);
	if (ssid_len < 1)
		return NULL;

	network = connman_network_create(group, CONNMAN_NETWORK_TYPE_WIFI);
	if (network == NULL)
		return NULL;

	connman_network_set_blob(network, "WiFi.SSID",
					(unsigned char *) ssid, ssid_len);

	connman_network_set_string(network, "WiFi.Mode", mode);
	connman_network_set_string(network, "WiFi.Security", security);

	name = g_try_malloc0(ssid_len + 1);
	if (name == NULL) {
		connman_network_unref(network);
		return NULL;
	}

	for (i = 0; i < ssid_len; i++) {
		if (g_ascii_isprint(ssid[i]))
			name[i] = ssid[i];
		else
			name[i] = ' ';
	}

	connman_network_set_name(network, name);

	g_free(name);

	index = connman_device_get_index(device);
	connman_network_set_index(network, index);

	if (connman_device_add_network(device, network) < 0) {
		connman_network_unref(network);
		return NULL;
	}

	connman_network_set_available(network, TRUE);

	return network;
}

int __connman_service_create_and_connect(DBusMessage *msg)
{
	struct connman_service *service;
	struct connman_network *network;
	struct connman_device *device;
	DBusMessageIter iter, array;
	const char *mode = "managed", *security = "none", *group_security;
	const char *type = NULL, *ssid = NULL, *passphrase = NULL;
	connman_bool_t network_created = FALSE;
	unsigned int ssid_len = 0;
	const char *ident;
	char *name, *group;
	int err;

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_recurse(&iter, &array);

	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(&array, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		switch (dbus_message_iter_get_arg_type(&value)) {
		case DBUS_TYPE_STRING:
			if (g_str_equal(key, "Type") == TRUE)
				dbus_message_iter_get_basic(&value, &type);
			else if (g_str_equal(key, "WiFi.Mode") == TRUE ||
					g_str_equal(key, "Mode") == TRUE)
				dbus_message_iter_get_basic(&value, &mode);
			else if (g_str_equal(key, "WiFi.Security") == TRUE ||
					g_str_equal(key, "Security") == TRUE)
				dbus_message_iter_get_basic(&value, &security);
			else if (g_str_equal(key, "WiFi.Passphrase") == TRUE ||
					g_str_equal(key, "Passphrase") == TRUE)
				dbus_message_iter_get_basic(&value, &passphrase);
			else if (g_str_equal(key, "WiFi.SSID") == TRUE ||
					g_str_equal(key, "SSID") == TRUE)
				dbus_message_iter_get_basic(&value, &ssid);
		}

		dbus_message_iter_next(&array);
	}

	if (type == NULL)
		return -EINVAL;

	if (g_strcmp0(type, "wifi") != 0 || g_strcmp0(mode, "managed") != 0)
		return -EOPNOTSUPP;

	if (ssid == NULL)
		return -EINVAL;

	ssid_len = strlen(ssid);
	if (ssid_len < 1)
		return -EINVAL;

	if (g_strcmp0(security, "none") != 0 &&
				g_strcmp0(security, "wep") != 0 &&
				g_strcmp0(security, "psk") != 0 &&
				g_strcmp0(security, "wpa") != 0 &&
				g_strcmp0(security, "rsn") != 0 &&
				g_strcmp0(security, "ieee8021x") != 0)
		return -EINVAL;

	device = __connman_device_find_device(CONNMAN_SERVICE_TYPE_WIFI);
	if (device == NULL)
		return -EOPNOTSUPP;

	ident = connman_device_get_ident(device);
	if (ident == NULL)
		return -EOPNOTSUPP;


	if (!g_strcmp0(security, "wpa") ||
		!g_strcmp0(security, "rsn"))
		group_security = "psk";
	else
		group_security = security;

	group = wifi_build_group_name((unsigned char *) ssid,
						ssid_len, mode, group_security);
	if (group == NULL)
		return -EINVAL;

	name = g_strdup_printf("%s_%s_%s", type, ident, group);

	service = lookup_by_identifier(name);

	if (service == NULL) {
		network = create_hidden_wifi(device, ssid,
						mode, security, group);
		if (network != NULL) {
			connman_network_set_group(network, group);
			network_created = TRUE;
		}

		service = lookup_by_identifier(name);
	}

	g_free(name);
	g_free(group);

	if (service == NULL) {
		err = -EOPNOTSUPP;
		goto failed;
	}

	service->network_created = network_created;

	if (is_connected(service) == TRUE) {
		err = -EISCONN;
		goto failed;
	}

	if (is_connecting(service) == TRUE) {
		err = -EALREADY;
		goto failed;
	}

	set_reconnect_state(service, FALSE);

	__connman_device_disconnect(device);

	if (passphrase != NULL) {
		g_free(service->passphrase);
		service->passphrase = g_strdup(passphrase);
	}

	service->userconnect = TRUE;

	err = __connman_service_connect(service);
	if (err < 0 && err != -EINPROGRESS)
		goto failed;

	service->pending = dbus_message_ref(msg);

	return 0;

failed:
	if (service != NULL && service->network_created == TRUE) {
		struct connman_network *network = service->network;

		if (network != NULL) {
			connman_network_set_available(network, FALSE);
			__connman_device_cleanup_networks(device);
		} else
			__connman_service_put(service);
	}

	return err;
}

static void provision_changed(gpointer value, gpointer user_data)
{
	struct connman_service *service = value;
	char *path = user_data;

	__connman_config_provision_service_ident(service, path);
}

void __connman_service_provision_changed(const char *ident)
{
	g_sequence_foreach(service_list, provision_changed, (void *)ident);
}

int __connman_service_provision(DBusMessage *msg)
{
	GKeyFile *keyfile = NULL;
	const char *config_str = NULL;
	char *group = NULL, *ident = NULL;
	int err = 0;
	struct connman_service *service;

	DBG("");

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &config_str,
							DBUS_TYPE_INVALID);

	if (config_str == NULL || strlen(config_str) == 0)
		return -EINVAL;

	keyfile = g_key_file_new();

	/* populate GKeyFile with config_str */
	if (g_key_file_load_from_data(keyfile, config_str,
					strlen(config_str), 0, NULL) == FALSE) {
		err = -EINVAL;
		goto done;
	}

	/*
	 * read only one group of settings (only one service supported, no
	 * global settings)
	 */
	group = g_key_file_get_start_group(keyfile);

	if (group == NULL || g_str_has_prefix(group, "service_") == FALSE) {
		err = -EINVAL;
		goto done;
	}

	err = __connman_config_load_service(keyfile, group, TRUE);
	if (err < 0)
		goto done;

	ident = group + strlen("service_");

	/* trigger service provisioning if service exists */
	service = lookup_by_identifier(ident);
	if (service != NULL)
		__connman_config_provision_service(service);

	g_dbus_send_reply(connection, msg, DBUS_TYPE_INVALID);

done:
	if (group != NULL)
		g_free(group);

	if (keyfile != NULL)
		g_key_file_free(keyfile);

	return err;
}

/**
 * __connman_service_get:
 * @identifier: service identifier
 *
 * Look up a service by identifier or create a new one if not found
 */
static struct connman_service *service_get(const char *identifier)
{
	struct connman_service *service;
	GSequenceIter *iter;

	iter = g_hash_table_lookup(service_hash, identifier);
	if (iter != NULL) {
		service = g_sequence_get(iter);
		if (service != NULL)
			connman_service_ref(service);
		return service;
	}

	service = connman_service_create();
	if (service == NULL)
		return NULL;

	DBG("service %p", service);

	service->identifier = g_strdup(identifier);

	iter = g_sequence_insert_sorted(service_list, service,
						service_compare, NULL);

	g_hash_table_insert(service_hash, service->identifier, iter);

	return service;
}

static int service_register(struct connman_service *service)
{
	GSequenceIter *iter;

	DBG("service %p", service);

	if (service->path != NULL)
		return -EALREADY;

	service->path = g_strdup_printf("%s/service/%s", CONNMAN_PATH,
						service->identifier);

	DBG("path %s", service->path);

	__connman_config_provision_service(service);

	service_load(service);

	g_dbus_register_interface(connection, service->path,
					CONNMAN_SERVICE_INTERFACE,
					service_methods, service_signals,
							NULL, service, NULL);

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter != NULL)
		g_sequence_sort_changed(iter, service_compare, NULL);

	services_changed(TRUE);

	return 0;
}

static void service_up(struct connman_ipconfig *ipconfig)
{
	struct connman_service *service = connman_ipconfig_get_data(ipconfig);

	DBG("%s up", connman_ipconfig_get_ifname(ipconfig));

	link_changed(service);

	service->stats.valid = FALSE;
	service->stats_roaming.valid = FALSE;
}

static void service_down(struct connman_ipconfig *ipconfig)
{
	DBG("%s down", connman_ipconfig_get_ifname(ipconfig));
}

static void service_lower_up(struct connman_ipconfig *ipconfig)
{
	struct connman_service *service = connman_ipconfig_get_data(ipconfig);

	DBG("%s lower up", connman_ipconfig_get_ifname(ipconfig));

	stats_start(service);
}

static void service_lower_down(struct connman_ipconfig *ipconfig)
{
	struct connman_service *service = connman_ipconfig_get_data(ipconfig);

	DBG("%s lower down", connman_ipconfig_get_ifname(ipconfig));

	stats_stop(service);
	service_save(service);
}

static void service_ip_bound(struct connman_ipconfig *ipconfig)
{
	struct connman_service *service = connman_ipconfig_get_data(ipconfig);
	enum connman_ipconfig_method method = CONNMAN_IPCONFIG_METHOD_UNKNOWN;
	enum connman_ipconfig_type type = CONNMAN_IPCONFIG_TYPE_UNKNOWN;

	DBG("%s ip bound", connman_ipconfig_get_ifname(ipconfig));

	type = __connman_ipconfig_get_config_type(ipconfig);
	method = __connman_ipconfig_get_method(ipconfig);

	DBG("service %p ipconfig %p type %d method %d", service, ipconfig,
							type, method);

	if (type == CONNMAN_IPCONFIG_TYPE_IPV6 &&
			method == CONNMAN_IPCONFIG_METHOD_AUTO)
		__connman_service_ipconfig_indicate_state(service,
						CONNMAN_SERVICE_STATE_READY,
						CONNMAN_IPCONFIG_TYPE_IPV6);

	settings_changed(service, ipconfig);
}

static void service_ip_release(struct connman_ipconfig *ipconfig)
{
	struct connman_service *service = connman_ipconfig_get_data(ipconfig);
	enum connman_ipconfig_method method = CONNMAN_IPCONFIG_METHOD_UNKNOWN;
	enum connman_ipconfig_type type = CONNMAN_IPCONFIG_TYPE_UNKNOWN;

	DBG("%s ip release", connman_ipconfig_get_ifname(ipconfig));

	type = __connman_ipconfig_get_config_type(ipconfig);
	method = __connman_ipconfig_get_method(ipconfig);

	DBG("service %p ipconfig %p type %d method %d", service, ipconfig,
							type, method);

	if (type == CONNMAN_IPCONFIG_TYPE_IPV6 &&
			method == CONNMAN_IPCONFIG_METHOD_OFF)
		__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_DISCONNECT,
					CONNMAN_IPCONFIG_TYPE_IPV6);

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4 &&
			method == CONNMAN_IPCONFIG_METHOD_OFF)
		__connman_service_ipconfig_indicate_state(service,
					CONNMAN_SERVICE_STATE_DISCONNECT,
					CONNMAN_IPCONFIG_TYPE_IPV4);

	settings_changed(service, ipconfig);
}

static const struct connman_ipconfig_ops service_ops = {
	.up		= service_up,
	.down		= service_down,
	.lower_up	= service_lower_up,
	.lower_down	= service_lower_down,
	.ip_bound	= service_ip_bound,
	.ip_release	= service_ip_release,
};

static void setup_ip4config(struct connman_service *service, int index,
			enum connman_ipconfig_method method)
{
	if (index < 0)
		return;

	service->ipconfig_ipv4 = connman_ipconfig_create(index,
						CONNMAN_IPCONFIG_TYPE_IPV4);
	if (service->ipconfig_ipv4 == NULL)
		return;

	connman_ipconfig_set_method(service->ipconfig_ipv4, method);

	connman_ipconfig_set_data(service->ipconfig_ipv4, service);

	connman_ipconfig_set_ops(service->ipconfig_ipv4, &service_ops);
}

static void setup_ip6config(struct connman_service *service, int index)
{
	if (index < 0)
		return;

	service->ipconfig_ipv6 = connman_ipconfig_create(index,
						CONNMAN_IPCONFIG_TYPE_IPV6);
	if (service->ipconfig_ipv6 == NULL)
		return;

	connman_ipconfig_set_data(service->ipconfig_ipv6, service);

	connman_ipconfig_set_ops(service->ipconfig_ipv6, &service_ops);
}

void __connman_service_read_ip4config(struct connman_service *service)
{
	GKeyFile *keyfile;

	if (service->ipconfig_ipv4 == NULL)
		return;

	keyfile = connman_storage_load_service(service->identifier);
	if (keyfile == NULL)
		return;

	__connman_ipconfig_load(service->ipconfig_ipv4, keyfile,
				service->identifier, "IPv4.");

	g_key_file_free(keyfile);
}

void __connman_service_create_ip4config(struct connman_service *service,
					int index)
{
	DBG("ipv4 %p", service->ipconfig_ipv4);

	if (service->ipconfig_ipv4 != NULL)
		return;

	setup_ip4config(service, index, CONNMAN_IPCONFIG_METHOD_DHCP);
	__connman_service_read_ip4config(service);
}

void __connman_service_read_ip6config(struct connman_service *service)
{
	GKeyFile *keyfile;

	if (service->ipconfig_ipv6 == NULL)
		return;

	keyfile = connman_storage_load_service(service->identifier);
	if (keyfile == NULL)
		return;

	__connman_ipconfig_load(service->ipconfig_ipv6, keyfile,
				service->identifier, "IPv6.");

	g_key_file_free(keyfile);
}

void __connman_service_create_ip6config(struct connman_service *service,
								int index)
{
	DBG("ipv6 %p", service->ipconfig_ipv6);

	if (service->ipconfig_ipv6 != NULL)
		return;

	setup_ip6config(service, index);

	__connman_service_read_ip6config(service);
}

/**
 * __connman_service_lookup_from_network:
 * @network: network structure
 *
 * Look up a service by network (reference count will not be increased)
 */
struct connman_service *__connman_service_lookup_from_network(struct connman_network *network)
{
	struct connman_service *service;
	const char *ident, *group;
	char *name;

	DBG("network %p", network);

	if (network == NULL)
		return NULL;

	ident = __connman_network_get_ident(network);
	if (ident == NULL)
		return NULL;

	group = connman_network_get_group(network);
	if (group == NULL)
		return NULL;

	name = g_strdup_printf("%s_%s_%s",
			__connman_network_get_type(network), ident, group);
	service = lookup_by_identifier(name);
	g_free(name);

	return service;
}

struct connman_service *__connman_service_lookup_from_index(int index)
{
	struct connman_service *service;
	GSequenceIter *iter;

	iter = g_sequence_get_begin_iter(service_list);

	while (g_sequence_iter_is_end(iter) == FALSE) {
		service = g_sequence_get(iter);

		if (connman_ipconfig_get_index(service->ipconfig_ipv4)
							== index)
			return service;

		if (connman_ipconfig_get_index(service->ipconfig_ipv6)
							== index)
			return service;

		iter = g_sequence_iter_next(iter);
	}

	return NULL;
}

const char *__connman_service_get_ident(struct connman_service *service)
{
	return service->identifier;
}

const char *__connman_service_get_path(struct connman_service *service)
{
	return service->path;
}

unsigned int __connman_service_get_order(struct connman_service *service)
{
	GSequenceIter *iter;

	if (service == NULL)
		return 0;

	if (service->favorite == FALSE) {
		service->order = 0;
		goto done;
	}

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter != NULL) {
		if (g_sequence_iter_get_position(iter) == 0)
			service->order = 1;
		else if (service->type == CONNMAN_SERVICE_TYPE_VPN)
			service->order = 10;
		else
			service->order = 0;
	}

done:
	return service->order;
}

static enum connman_service_type convert_network_type(struct connman_network *network)
{
	enum connman_network_type type = connman_network_get_type(network);

	switch (type) {
	case CONNMAN_NETWORK_TYPE_UNKNOWN:
	case CONNMAN_NETWORK_TYPE_VENDOR:
		break;
	case CONNMAN_NETWORK_TYPE_ETHERNET:
		return CONNMAN_SERVICE_TYPE_ETHERNET;
	case CONNMAN_NETWORK_TYPE_WIFI:
		return CONNMAN_SERVICE_TYPE_WIFI;
	case CONNMAN_NETWORK_TYPE_WIMAX:
		return CONNMAN_SERVICE_TYPE_WIMAX;
	case CONNMAN_NETWORK_TYPE_BLUETOOTH_PAN:
	case CONNMAN_NETWORK_TYPE_BLUETOOTH_DUN:
		return CONNMAN_SERVICE_TYPE_BLUETOOTH;
	case CONNMAN_NETWORK_TYPE_CELLULAR:
		return CONNMAN_SERVICE_TYPE_CELLULAR;
	}

	return CONNMAN_SERVICE_TYPE_UNKNOWN;
}

static enum connman_service_security convert_wifi_security(const char *security)
{
	if (security == NULL)
		return CONNMAN_SERVICE_SECURITY_UNKNOWN;
	else if (g_str_equal(security, "none") == TRUE)
		return CONNMAN_SERVICE_SECURITY_NONE;
	else if (g_str_equal(security, "wep") == TRUE)
		return CONNMAN_SERVICE_SECURITY_WEP;
	else if (g_str_equal(security, "psk") == TRUE)
		return CONNMAN_SERVICE_SECURITY_PSK;
	else if (g_str_equal(security, "ieee8021x") == TRUE)
		return CONNMAN_SERVICE_SECURITY_8021X;
	else if (g_str_equal(security, "wpa") == TRUE)
		return CONNMAN_SERVICE_SECURITY_WPA;
	else if (g_str_equal(security, "rsn") == TRUE)
		return CONNMAN_SERVICE_SECURITY_RSN;
	else
		return CONNMAN_SERVICE_SECURITY_UNKNOWN;
}

static void update_from_network(struct connman_service *service,
					struct connman_network *network)
{
	connman_uint8_t strength = service->strength;
	GSequenceIter *iter;
	const char *str;

	DBG("service %p network %p", service, network);

	if (is_connected(service) == TRUE)
		return;

	if (is_connecting(service) == TRUE)
		return;

	str = connman_network_get_string(network, "Name");
	if (str != NULL) {
		g_free(service->name);
		service->name = g_strdup(str);
		service->hidden = FALSE;
	} else {
		g_free(service->name);
		service->name = NULL;
		service->hidden = TRUE;
	}

	service->strength = connman_network_get_strength(network);
	service->roaming = connman_network_get_bool(network, "Roaming");

	if (service->strength == 0) {
		/*
		 * Filter out 0-values; it's unclear what they mean
		 * and they cause anomalous sorting of the priority list.
		 */
		service->strength = strength;
	}

	str = connman_network_get_string(network, "WiFi.Security");
	service->security = convert_wifi_security(str);

	if (service->type == CONNMAN_SERVICE_TYPE_WIFI)
		service->wps = connman_network_get_bool(network, "WiFi.WPS");

	if (service->strength > strength && service->network != NULL) {
		service->network = network;

		strength_changed(service);
	}

	if (service->network == NULL)
		service->network = network;

	iter = g_hash_table_lookup(service_hash, service->identifier);
	if (iter != NULL)
		g_sequence_sort_changed(iter, service_compare, NULL);
}

/**
 * __connman_service_create_from_network:
 * @network: network structure
 *
 * Look up service by network and if not found, create one
 */
struct connman_service * __connman_service_create_from_network(struct connman_network *network)
{
	struct connman_service *service;
	struct connman_device *device;
	const char *ident, *group;
	char *name;
	int index;

	DBG("network %p", network);

	if (network == NULL)
		return NULL;

	ident = __connman_network_get_ident(network);
	if (ident == NULL)
		return NULL;

	group = connman_network_get_group(network);
	if (group == NULL)
		return NULL;

	name = g_strdup_printf("%s_%s_%s",
			__connman_network_get_type(network), ident, group);
	service = service_get(name);
	g_free(name);

	if (service == NULL)
		return NULL;

	if (__connman_network_get_weakness(network) == TRUE)
		return service;

	if (service->path != NULL) {
		update_from_network(service, network);
		services_changed(TRUE);
		return service;
	}

	service->type = convert_network_type(network);

	switch (service->type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		service->autoconnect = FALSE;
		break;
	case CONNMAN_SERVICE_TYPE_WIFI:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		service->autoconnect = TRUE;
		break;
	}

	service->state_ipv4 = service->state_ipv6 = CONNMAN_SERVICE_STATE_IDLE;
	service->state = combine_state(service->state_ipv4, service->state_ipv6);

	update_from_network(service, network);

	index = connman_network_get_index(network);

	if (service->ipconfig_ipv4 == NULL)
		setup_ip4config(service, index, CONNMAN_IPCONFIG_METHOD_DHCP);

	if (service->ipconfig_ipv6 == NULL)
		setup_ip6config(service, index);

	service_register(service);

	if (service->favorite == TRUE) {
		device = connman_network_get_device(service->network);
		if (device && __connman_device_scanning(device) == FALSE)
			__connman_service_auto_connect();
	}

	__connman_notifier_service_add(service, service->name);

	return service;
}

void __connman_service_update_from_network(struct connman_network *network)
{
	connman_bool_t need_sort = FALSE;
	struct connman_service *service;
	connman_uint8_t strength;
	connman_bool_t roaming;
	GSequenceIter *iter;
	const char *name;
	connman_bool_t stats_enable;

	DBG("network %p", network);

	service = __connman_service_lookup_from_network(network);
	if (service == NULL)
		return;

	if (service->network == NULL)
		return;

	name = connman_network_get_string(service->network, "Name");
	if (g_strcmp0(service->name, name) != 0) {
		g_free(service->name);
		service->name = g_strdup(name);
		connman_dbus_property_changed_basic(service->path,
				CONNMAN_SERVICE_INTERFACE, "Name",
				DBUS_TYPE_STRING, &service->name);
	}

	if (service->type == CONNMAN_SERVICE_TYPE_WIFI)
		service->wps = connman_network_get_bool(network, "WiFi.WPS");

	strength = connman_network_get_strength(service->network);
	if (strength == service->strength)
		goto roaming;

	service->strength = strength;
	need_sort = TRUE;

	strength_changed(service);

roaming:
	roaming = connman_network_get_bool(service->network, "Roaming");
	if (roaming == service->roaming)
		goto sorting;

	stats_enable = stats_enabled(service);
	if (stats_enable == TRUE)
		stats_stop(service);

	service->roaming = roaming;
	need_sort = TRUE;

	if (stats_enable == TRUE)
		stats_start(service);

	roaming_changed(service);

sorting:
	if (need_sort == TRUE) {
		iter = g_hash_table_lookup(service_hash, service->identifier);
		if (iter != NULL)
			g_sequence_sort_changed(iter, service_compare, NULL);
	}
}

void __connman_service_remove_from_network(struct connman_network *network)
{
	struct connman_service *service;

	DBG("network %p", network);

	service = __connman_service_lookup_from_network(network);
	if (service == NULL)
		return;

	__connman_connection_gateway_remove(service,
					CONNMAN_IPCONFIG_TYPE_ALL);

	__connman_service_put(service);
}

/**
 * __connman_service_create_from_provider:
 * @provider: provider structure
 *
 * Look up service by provider and if not found, create one
 */
struct connman_service *
__connman_service_create_from_provider(struct connman_provider *provider)
{
	struct connman_service *service;
	const char *ident, *str;
	char *name;
	int index = connman_provider_get_index(provider);

	DBG("provider %p", provider);

	ident = __connman_provider_get_ident(provider);
	if (ident == NULL)
		return NULL;

	name = g_strdup_printf("vpn_%s", ident);
	service = service_get(name);
	g_free(name);

	if (service == NULL)
		return NULL;

	service->type = CONNMAN_SERVICE_TYPE_VPN;
	service->provider = connman_provider_ref(provider);
	service->autoconnect = FALSE;
	service->userconnect = TRUE;

	service->state_ipv4 = service->state_ipv6 = CONNMAN_SERVICE_STATE_IDLE;

	str = connman_provider_get_string(provider, "Name");
	if (str != NULL) {
		g_free(service->name);
		service->name = g_strdup(str);
		service->hidden = FALSE;
	} else {
		g_free(service->name);
		service->name = NULL;
		service->hidden = TRUE;
	}

	service->strength = 0;

	if (service->ipconfig_ipv4 == NULL)
		setup_ip4config(service, index, CONNMAN_IPCONFIG_METHOD_MANUAL);

	if (service->ipconfig_ipv6 == NULL)
		setup_ip6config(service, index);

	service_register(service);

	__connman_notifier_service_add(service, service->name);

	return service;
}

int __connman_service_init(void)
{
	DBG("");

	connection = connman_dbus_get_connection();

	service_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
								NULL, NULL);

	service_list = g_sequence_new(service_free);

	return 0;
}

void __connman_service_cleanup(void)
{
	GSequence *list;

	DBG("");

	list = service_list;
	service_list = NULL;
	g_sequence_free(list);

	g_hash_table_destroy(service_hash);
	service_hash = NULL;

	g_slist_free(counter_list);
	counter_list = NULL;

	dbus_connection_unref(connection);
}
