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

#ifndef __CONNMAN_PROVIDER_H
#define __CONNMAN_PROVIDER_H

#include <glib.h>
#include <connman/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SECTION:provider
 * @title: Provider premitives
 * @short_description: Functions for handling providers
 */

enum connman_provider_type {
	CONNMAN_PROVIDER_TYPE_UNKNOWN = 0,
	CONNMAN_PROVIDER_TYPE_VPN     = 1,
};

enum connman_provider_state {
	CONNMAN_PROVIDER_STATE_UNKNOWN       = 0,
	CONNMAN_PROVIDER_STATE_IDLE          = 1,
	CONNMAN_PROVIDER_STATE_CONNECT       = 2,
	CONNMAN_PROVIDER_STATE_READY         = 3,
	CONNMAN_PROVIDER_STATE_DISCONNECT    = 4,
	CONNMAN_PROVIDER_STATE_FAILURE       = 5,
};

enum connman_provider_error {
	CONNMAN_PROVIDER_ERROR_UNKNOWN		= 0,
	CONNMAN_PROVIDER_ERROR_CONNECT_FAILED	= 1,
	CONNMAN_PROVIDER_ERROR_LOGIN_FAILED	= 2,
	CONNMAN_PROVIDER_ERROR_AUTH_FAILED	= 3,
};

struct connman_provider;
struct connman_ipaddress;

struct connman_provider *connman_provider_ref(struct connman_provider *provider);
void connman_provider_unref(struct connman_provider *provider);

int connman_provider_set_string(struct connman_provider *provider,
					const char *key, const char *value);
const char *connman_provider_get_string(struct connman_provider *provider,
							const char *key);

int connman_provider_set_state(struct connman_provider *provider,
					enum connman_provider_state state);

int connman_provider_indicate_error(struct connman_provider *provider,
					enum connman_provider_error error);

void connman_provider_set_index(struct connman_provider *provider, int index);
int connman_provider_get_index(struct connman_provider *provider);

void connman_provider_set_data(struct connman_provider *provider, void *data);
void *connman_provider_get_data(struct connman_provider *provider);
int connman_provider_set_ipaddress(struct connman_provider *provider,
					struct connman_ipaddress *ipaddress);
int connman_provider_set_pac(struct connman_provider *provider,
				const char *pac);
int connman_provider_set_domain(struct connman_provider *provider,
				const char *domain);
int connman_provider_set_nameservers(struct connman_provider *provider,
					const char *nameservers);
int connman_provider_append_route(struct connman_provider *provider,
					const char *key, const char *value);

const char *connman_provider_get_driver_name(struct connman_provider *provider);
const char *connman_provider_get_save_group(struct connman_provider *provider);

struct connman_provider_driver {
	const char *name;
	enum connman_provider_type type;
	int (*probe) (struct connman_provider *provider);
	int (*remove) (struct connman_provider *provider);
	int (*connect) (struct connman_provider *provider);
	int (*disconnect) (struct connman_provider *provider);
	int (*save) (struct connman_provider *provider, GKeyFile *keyfile);
};

int connman_provider_driver_register(struct connman_provider_driver *driver);
void connman_provider_driver_unregister(struct connman_provider_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __CONNMAN_PROVIDER_H */
