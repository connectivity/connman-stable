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

#include <gdbus.h>

#include "connman.h"

connman_bool_t connman_state_idle;
DBusMessage *session_mode_pending = NULL;

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter array, dict;
	connman_bool_t offlinemode, sessionmode;
	const char *str;

	DBG("conn %p", conn);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &array);

	connman_dbus_dict_open(&array, &dict);

	connman_dbus_dict_append_array(&dict, "Services",
			DBUS_TYPE_OBJECT_PATH, __connman_service_list, NULL);
	connman_dbus_dict_append_array(&dict, "Technologies",
			DBUS_TYPE_OBJECT_PATH, __connman_technology_list, NULL);

	str = __connman_notifier_get_state();
	connman_dbus_dict_append_basic(&dict, "State",
						DBUS_TYPE_STRING, &str);

	offlinemode = __connman_technology_get_offlinemode();
	connman_dbus_dict_append_basic(&dict, "OfflineMode",
					DBUS_TYPE_BOOLEAN, &offlinemode);

	connman_dbus_dict_append_array(&dict, "AvailableTechnologies",
		DBUS_TYPE_STRING, __connman_notifier_list_registered, NULL);
	connman_dbus_dict_append_array(&dict, "EnabledTechnologies",
		DBUS_TYPE_STRING, __connman_notifier_list_enabled, NULL);
	connman_dbus_dict_append_array(&dict, "ConnectedTechnologies",
		DBUS_TYPE_STRING, __connman_notifier_list_connected, NULL);

	str = __connman_service_default();
	if (str != NULL)
		connman_dbus_dict_append_basic(&dict, "DefaultTechnology",
						DBUS_TYPE_STRING, &str);

	connman_dbus_dict_append_array(&dict, "AvailableDebugs",
			DBUS_TYPE_STRING, __connman_debug_list_available, NULL);
	connman_dbus_dict_append_array(&dict, "EnabledDebugs",
			DBUS_TYPE_STRING, __connman_debug_list_enabled, NULL);

	sessionmode = __connman_session_mode();
	connman_dbus_dict_append_basic(&dict, "SessionMode",
					DBUS_TYPE_BOOLEAN,
					&sessionmode);

	connman_dbus_dict_close(&array, &dict);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessageIter iter, value;
	const char *name;
	int type;

	DBG("conn %p", conn);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __connman_error_invalid_arguments(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	type = dbus_message_iter_get_arg_type(&value);

	if (g_str_equal(name, "OfflineMode") == TRUE) {
		connman_bool_t offlinemode;

		if (type != DBUS_TYPE_BOOLEAN)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &offlinemode);

		__connman_technology_set_offlinemode(offlinemode);
	} else if (g_str_equal(name, "SessionMode") == TRUE) {
		connman_bool_t sessionmode;

		if (type != DBUS_TYPE_BOOLEAN)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &sessionmode);

		if (session_mode_pending != NULL)
			return __connman_error_in_progress(msg);

		__connman_session_set_mode(sessionmode);

		if (sessionmode == TRUE && connman_state_idle == FALSE) {
			session_mode_pending = msg;
			return NULL;
		}

	} else
		return __connman_error_invalid_property(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *get_state(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *str;

	DBG("conn %p", conn);

	str = __connman_notifier_get_state();

	return g_dbus_create_reply(msg, DBUS_TYPE_STRING, &str,
						DBUS_TYPE_INVALID);
}

static DBusMessage *remove_provider(DBusConnection *conn,
				    DBusMessage *msg, void *data)
{
	const char *path;
	int err;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	err = __connman_provider_remove(path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *request_scan(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	enum connman_service_type type;
	const char *str;
	int err;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &str,
							DBUS_TYPE_INVALID);

	if (g_strcmp0(str, "") == 0)
		type = CONNMAN_SERVICE_TYPE_UNKNOWN;
	else if (g_strcmp0(str, "wifi") == 0)
		type = CONNMAN_SERVICE_TYPE_WIFI;
	else if (g_strcmp0(str, "wimax") == 0)
		type = CONNMAN_SERVICE_TYPE_WIMAX;
	else
		return __connman_error_invalid_arguments(msg);

	err = __connman_device_request_scan(type);
	if (err < 0) {
		if (err == -EINPROGRESS) {
			connman_error("Invalid return code from scan");
			err = -EINVAL;
		}

		return __connman_error_failed(msg, -err);
	}

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusConnection *connection = NULL;

static void session_mode_notify(void)
{
	DBusMessage *reply;

	reply = g_dbus_create_reply(session_mode_pending, DBUS_TYPE_INVALID);
	g_dbus_send_message(connection, reply);

	dbus_message_unref(session_mode_pending);
	session_mode_pending = NULL;
}

static void idle_state(connman_bool_t idle)
{

	DBG("idle %d", idle);

	connman_state_idle = idle;

	if (connman_state_idle == FALSE || session_mode_pending == NULL)
		return;

	session_mode_notify();
}

static struct connman_notifier technology_notifier = {
	.name		= "manager",
	.priority	= CONNMAN_NOTIFIER_PRIORITY_HIGH,
	.idle_state	= idle_state,
};

static DBusMessage *enable_technology(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	enum connman_service_type type;
	const char *str;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &str,
							DBUS_TYPE_INVALID);

	if (g_strcmp0(str, "ethernet") == 0)
		type = CONNMAN_SERVICE_TYPE_ETHERNET;
	else if (g_strcmp0(str, "wifi") == 0)
		type = CONNMAN_SERVICE_TYPE_WIFI;
	else if (g_strcmp0(str, "wimax") == 0)
		type = CONNMAN_SERVICE_TYPE_WIMAX;
	else if (g_strcmp0(str, "bluetooth") == 0)
		type = CONNMAN_SERVICE_TYPE_BLUETOOTH;
	else if (g_strcmp0(str, "cellular") == 0)
		type = CONNMAN_SERVICE_TYPE_CELLULAR;
	else
		return __connman_error_invalid_arguments(msg);

	if (__connman_notifier_is_registered(type) == FALSE)
		return __connman_error_not_registered(msg);

	if (__connman_notifier_is_enabled(type) == TRUE)
		return __connman_error_already_enabled(msg);

	 __connman_technology_enable(type, msg);

	return NULL;
}

static DBusMessage *disable_technology(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	enum connman_service_type type;
	const char *str;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &str,
							DBUS_TYPE_INVALID);

	if (g_strcmp0(str, "ethernet") == 0)
		type = CONNMAN_SERVICE_TYPE_ETHERNET;
	else if (g_strcmp0(str, "wifi") == 0)
		type = CONNMAN_SERVICE_TYPE_WIFI;
	else if (g_strcmp0(str, "wimax") == 0)
		type = CONNMAN_SERVICE_TYPE_WIMAX;
	else if (g_strcmp0(str, "bluetooth") == 0)
		type = CONNMAN_SERVICE_TYPE_BLUETOOTH;
	else if (g_strcmp0(str, "cellular") == 0)
		type = CONNMAN_SERVICE_TYPE_CELLULAR;
	else
		return __connman_error_invalid_arguments(msg);

	if (__connman_notifier_is_registered(type) == FALSE)
		return __connman_error_not_registered(msg);

	if (__connman_notifier_is_enabled(type) == FALSE)
		return __connman_error_already_disabled(msg);

	__connman_technology_disable(type, msg);

	return NULL;
}

static DBusMessage *get_services(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter, array;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_STRUCT_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_OBJECT_PATH_AS_STRING
			DBUS_TYPE_ARRAY_AS_STRING
				DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
				DBUS_DICT_ENTRY_END_CHAR_AS_STRING
			DBUS_STRUCT_END_CHAR_AS_STRING, &array);

	__connman_service_list_struct(&array);

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static DBusMessage *lookup_service(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *pattern, *path;
	int err;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID);

	err = __connman_service_lookup(pattern, &path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);
}

static DBusMessage *connect_service(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	int err;

	DBG("conn %p", conn);

	if (__connman_session_mode() == TRUE) {
		connman_info("Session mode enabled: "
				"direct service connect disabled");

		return __connman_error_failed(msg, -EINVAL);
	}

	err = __connman_service_create_and_connect(msg);
	if (err < 0) {
		if (err == -EINPROGRESS) {
			connman_error("Invalid return code from connect");
			err = -EINVAL;
		}

		return __connman_error_failed(msg, -err);
	}

	return NULL;
}

static DBusMessage *provision_service(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	int err;

	DBG("conn %p", conn);

	err = __connman_service_provision(msg);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return NULL;
}

static DBusMessage *connect_provider(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	int err;

	DBG("conn %p", conn);

	if (__connman_session_mode() == TRUE) {
		connman_info("Session mode enabled: "
				"direct provider connect disabled");

		return __connman_error_failed(msg, -EINVAL);
	}

	err = __connman_provider_create_and_connect(msg);
	if (err < 0) {
		if (err == -EINPROGRESS) {
			connman_error("Invalid return code from connect");
			err = -EINVAL;
		}

		return __connman_error_failed(msg, -err);
	}

	return NULL;
}

static DBusMessage *register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *sender, *path;
	int err;

	DBG("conn %p", conn);

	sender = dbus_message_get_sender(msg);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	err = __connman_agent_register(sender, path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *unregister_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *sender, *path;
	int err;

	DBG("conn %p", conn);

	sender = dbus_message_get_sender(msg);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	err = __connman_agent_unregister(sender, path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *register_counter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *sender, *path;
	unsigned int accuracy, period;
	int err;

	DBG("conn %p", conn);

	sender = dbus_message_get_sender(msg);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_UINT32, &accuracy,
						DBUS_TYPE_UINT32, &period,
							DBUS_TYPE_INVALID);

	/* FIXME: add handling of accuracy parameter */

	err = __connman_counter_register(sender, path, period);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *unregister_counter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *sender, *path;
	int err;

	DBG("conn %p", conn);

	sender = dbus_message_get_sender(msg);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	err = __connman_counter_unregister(sender, path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *create_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	int err;

	DBG("conn %p", conn);

	err = __connman_session_create(msg);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *destroy_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	int err;

	DBG("conn %p", conn);

	err = __connman_session_destroy(msg);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *request_private_network(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *sender;
	int  err;

	DBG("conn %p", conn);

	sender = dbus_message_get_sender(msg);

	err = __connman_private_network_request(msg, sender);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return NULL;
}

static DBusMessage *release_private_network(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *path;
	int err;

	DBG("conn %p", conn);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	err = __connman_private_network_release(path);
	if (err < 0)
		return __connman_error_failed(msg, -err);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",     "",      "a{sv}", get_properties     },
	{ "SetProperty",       "sv",    "",      set_property,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetState",          "",      "s",     get_state          },
	{ "RemoveProvider",    "o",     "",      remove_provider    },
	{ "RequestScan",       "s",     "",      request_scan       },
	{ "EnableTechnology",  "s",     "",      enable_technology,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "DisableTechnology", "s",     "",      disable_technology,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetServices",       "",      "a(oa{sv})", get_services   },
	{ "LookupService",     "s",     "o",     lookup_service,    },
	{ "ConnectService",    "a{sv}", "o",     connect_service,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "ProvisionService",  "s",     "",      provision_service,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "ConnectProvider",   "a{sv}", "o",     connect_provider,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "RegisterAgent",     "o",     "",      register_agent     },
	{ "UnregisterAgent",   "o",     "",      unregister_agent   },
	{ "RegisterCounter",   "ouu",   "",      register_counter   },
	{ "UnregisterCounter", "o",     "",      unregister_counter },
	{ "CreateSession",     "a{sv}o", "o",    create_session     },
	{ "DestroySession",    "o",     "",      destroy_session    },
	{ "RequestPrivateNetwork",    "",     "oa{sv}h",
						request_private_network,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "ReleasePrivateNetwork",    "o",    "",
						release_private_network },
	{ },
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged", "sv" },
	{ "StateChanged",    "s"  },
	{ },
};

int __connman_manager_init(void)
{
	DBG("");

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -1;

	if (connman_notifier_register(&technology_notifier) < 0)
		connman_error("Failed to register technology notifier");

	g_dbus_register_interface(connection, CONNMAN_MANAGER_PATH,
					CONNMAN_MANAGER_INTERFACE,
					manager_methods,
					manager_signals, NULL, NULL, NULL);

	connman_state_idle = TRUE;

	return 0;
}

void __connman_manager_cleanup(void)
{
	DBG("");

	if (connection == NULL)
		return;

	connman_notifier_unregister(&technology_notifier);

	g_dbus_unregister_interface(connection, CONNMAN_MANAGER_PATH,
						CONNMAN_MANAGER_INTERFACE);

	dbus_connection_unref(connection);
}
