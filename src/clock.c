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

#include <sys/time.h>

#include <gdbus.h>

#include "connman.h"

enum time_updates {
	TIME_UPDATES_UNKNOWN = 0,
	TIME_UPDATES_MANUAL  = 1,
	TIME_UPDATES_AUTO    = 2,
};

enum timezone_updates {
	TIMEZONE_UPDATES_UNKNOWN = 0,
	TIMEZONE_UPDATES_MANUAL  = 1,
	TIMEZONE_UPDATES_AUTO    = 2,
};

static enum time_updates time_updates_config = TIME_UPDATES_AUTO;
static enum timezone_updates timezone_updates_config = TIMEZONE_UPDATES_AUTO;

static char *timezone_config = NULL;
static char **timeservers_config = NULL;

static const char *time_updates2string(enum time_updates value)
{
	switch (value) {
	case TIME_UPDATES_UNKNOWN:
		break;
	case TIME_UPDATES_MANUAL:
		return "manual";
	case TIME_UPDATES_AUTO:
		return "auto";
	}

	return NULL;
}

static enum time_updates string2time_updates(const char *value)
{
	if (g_strcmp0(value, "manual") == 0)
		return TIME_UPDATES_MANUAL;
	else if (g_strcmp0(value, "auto") == 0)
		return TIME_UPDATES_AUTO;

	return TIME_UPDATES_UNKNOWN;
}

static const char *timezone_updates2string(enum timezone_updates value)
{
	switch (value) {
	case TIMEZONE_UPDATES_UNKNOWN:
		break;
	case TIMEZONE_UPDATES_MANUAL:
		return "manual";
	case TIMEZONE_UPDATES_AUTO:
		return "auto";
	}

	return NULL;
}

static enum timezone_updates string2timezone_updates(const char *value)
{
	if (g_strcmp0(value, "manual") == 0)
		return TIMEZONE_UPDATES_MANUAL;
	else if (g_strcmp0(value, "auto") == 0)
		return TIMEZONE_UPDATES_AUTO;

        return TIMEZONE_UPDATES_UNKNOWN;
}

static void append_timeservers(DBusMessageIter *iter, void *user_data)
{
	int i;

	if (timeservers_config == NULL)
		return;

	for (i = 0; timeservers_config[i] != NULL; i++) {
		dbus_message_iter_append_basic(iter,
				DBUS_TYPE_STRING, &timeservers_config[i]);
	}
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter array, dict;
	struct timeval tv;
	const char *str;

	DBG("conn %p", conn);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &array);

	connman_dbus_dict_open(&array, &dict);

	if (gettimeofday(&tv, NULL) == 0) {
		dbus_uint64_t val = tv.tv_sec;

		connman_dbus_dict_append_basic(&dict, "Time",
						DBUS_TYPE_UINT64, &val);
	}

	str = time_updates2string(time_updates_config);
	if (str != NULL)
		connman_dbus_dict_append_basic(&dict, "TimeUpdates",
						DBUS_TYPE_STRING, &str);

	if (timezone_config != NULL)
		connman_dbus_dict_append_basic(&dict, "Timezone",
					DBUS_TYPE_STRING, &timezone_config);

	str = timezone_updates2string(timezone_updates_config);
	if (str != NULL)
		connman_dbus_dict_append_basic(&dict, "TimezoneUpdates",
						DBUS_TYPE_STRING, &str);

	connman_dbus_dict_append_array(&dict, "Timeservers",
				DBUS_TYPE_STRING, append_timeservers, NULL);

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

	if (g_str_equal(name, "Time") == TRUE) {
		struct timeval tv;
		dbus_uint64_t newval;

		if (type != DBUS_TYPE_UINT64)
			return __connman_error_invalid_arguments(msg);

		if (time_updates_config != TIME_UPDATES_MANUAL)
			return __connman_error_permission_denied(msg);

		dbus_message_iter_get_basic(&value, &newval);

		tv.tv_sec = newval;
		tv.tv_usec = 0;

		if (settimeofday(&tv, NULL) < 0)
			return __connman_error_invalid_arguments(msg);

		connman_dbus_property_changed_basic(CONNMAN_MANAGER_PATH,
				CONNMAN_CLOCK_INTERFACE, "Time",
				DBUS_TYPE_UINT64, &newval);
	} else if (g_str_equal(name, "TimeUpdates") == TRUE) {
		const char *strval;
		enum time_updates newval;

		if (type != DBUS_TYPE_STRING)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &strval);
		newval = string2time_updates(strval);

		if (newval == TIME_UPDATES_UNKNOWN)
			return __connman_error_invalid_arguments(msg);

		if (newval == time_updates_config)
			return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);

		time_updates_config = newval;

		connman_dbus_property_changed_basic(CONNMAN_MANAGER_PATH,
				CONNMAN_CLOCK_INTERFACE, "TimeUpdates",
				DBUS_TYPE_STRING, &strval);
	} else if (g_str_equal(name, "Timezone") == TRUE) {
		const char *strval;

		if (type != DBUS_TYPE_STRING)
			return __connman_error_invalid_arguments(msg);

		if (timezone_updates_config != TIMEZONE_UPDATES_MANUAL)
			return __connman_error_permission_denied(msg);

		dbus_message_iter_get_basic(&value, &strval);

		if (__connman_timezone_change(strval) < 0)
                        return __connman_error_invalid_arguments(msg);
	} else if (g_str_equal(name, "TimezoneUpdates") == TRUE) {
		const char *strval;
		enum timezone_updates newval;

		if (type != DBUS_TYPE_STRING)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &strval);
		newval = string2timezone_updates(strval);

		if (newval == TIMEZONE_UPDATES_UNKNOWN)
			return __connman_error_invalid_arguments(msg);

		if (newval == timezone_updates_config)
			return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);

		timezone_updates_config = newval;

		connman_dbus_property_changed_basic(CONNMAN_MANAGER_PATH,
				CONNMAN_CLOCK_INTERFACE, "TimezoneUpdates",
				DBUS_TYPE_STRING, &strval);
	} else if (g_str_equal(name, "Timeservers") == TRUE) {
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

		g_strfreev(timeservers_config);

		if (str->len > 0)
			timeservers_config = g_strsplit_set(str->str, " ", 0);
		else
			timeservers_config = NULL;

		g_string_free(str, TRUE);

		connman_dbus_property_changed_array(CONNMAN_MANAGER_PATH,
				CONNMAN_CLOCK_INTERFACE, "Timeservers",
				DBUS_TYPE_STRING, append_timeservers, NULL);
	} else
		return __connman_error_invalid_property(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static GDBusMethodTable clock_methods[] = {
	{ "GetProperties", "",   "a{sv}", get_properties },
	{ "SetProperty",   "sv", "",      set_property   },
	{ },
};

static GDBusSignalTable clock_signals[] = {
	{ "PropertyChanged", "sv" },
	{ },
};

static DBusConnection *connection = NULL;

void __connman_clock_update_timezone(void)
{
	DBG("");

	g_free(timezone_config);
	timezone_config = __connman_timezone_lookup();

	if (timezone_config == NULL)
		return;

	connman_dbus_property_changed_basic(CONNMAN_MANAGER_PATH,
				CONNMAN_CLOCK_INTERFACE, "Timezone",
				DBUS_TYPE_STRING, &timezone_config);
}

int __connman_clock_init(void)
{
	DBG("");

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -1;

	__connman_timezone_init();

	timezone_config = __connman_timezone_lookup();

	g_dbus_register_interface(connection, CONNMAN_MANAGER_PATH,
						CONNMAN_CLOCK_INTERFACE,
						clock_methods, clock_signals,
						NULL, NULL, NULL);

	return 0;
}

void __connman_clock_cleanup(void)
{
	DBG("");

	if (connection == NULL)
		return;

	g_dbus_unregister_interface(connection, CONNMAN_MANAGER_PATH,
						CONNMAN_CLOCK_INTERFACE);

	dbus_connection_unref(connection);

	__connman_timezone_cleanup();

	g_free(timezone_config);
	g_strfreev(timeservers_config);
}
