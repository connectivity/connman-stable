/*
 *
 *  WPA supplicant library with GLib integration
 *
 *  Copyright (C) 2010  Intel Corporation. All rights reserved.
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
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "dbus.h"

#define TIMEOUT 5000

static DBusConnection *connection;

void supplicant_dbus_setup(DBusConnection *conn)
{
	connection = conn;
}

void supplicant_dbus_array_foreach(DBusMessageIter *iter,
				supplicant_dbus_array_function function,
							void *user_data)
{
	DBusMessageIter entry;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(iter, &entry);

	while (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_INVALID) {
		if (function != NULL)
			function(&entry, user_data);

		dbus_message_iter_next(&entry);
	}
}

void supplicant_dbus_property_foreach(DBusMessageIter *iter,
				supplicant_dbus_property_function function,
							void *user_data)
{
	DBusMessageIter dict;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return;

		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			return;

		dbus_message_iter_recurse(&entry, &value);

		if (key != NULL) {
			if (strcmp(key, "Properties") == 0)
				supplicant_dbus_property_foreach(&value,
							function, user_data);
			else if (function != NULL)
				function(key, &value, user_data);
		}

		dbus_message_iter_next(&dict);
	}
}

struct property_get_data {
	supplicant_dbus_property_function function;
	void *user_data;
};

static void property_get_all_reply(DBusPendingCall *call, void *user_data)
{
	struct property_get_data *data = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		goto done;

	if (dbus_message_iter_init(reply, &iter) == FALSE)
		goto done;

	supplicant_dbus_property_foreach(&iter, data->function,
							data->user_data);

	if (data->function != NULL)
		data->function(NULL, NULL, data->user_data);

done:
	dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

int supplicant_dbus_property_get_all(const char *path, const char *interface,
				supplicant_dbus_property_function function,
							void *user_data)
{
	struct property_get_data *data;
	DBusMessage *message;
	DBusPendingCall *call;

	if (connection == NULL)
		return -EINVAL;

	if (path == NULL || interface == NULL)
		return -EINVAL;

	data = dbus_malloc0(sizeof(*data));
	if (data == NULL)
		return -ENOMEM;

	message = dbus_message_new_method_call(SUPPLICANT_SERVICE, path,
					DBUS_INTERFACE_PROPERTIES, "GetAll");
	if (message == NULL) {
		dbus_free(data);
		return -ENOMEM;
	}

	dbus_message_set_auto_start(message, FALSE);

	dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, NULL);

	if (dbus_connection_send_with_reply(connection, message,
						&call, TIMEOUT) == FALSE) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	if (call == NULL) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	data->function = function;
	data->user_data = user_data;

	dbus_pending_call_set_notify(call, property_get_all_reply,
							data, dbus_free);

	dbus_message_unref(message);

	return 0;
}

static void property_get_reply(DBusPendingCall *call, void *user_data)
{
	struct property_get_data *data = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		goto done;

	if (dbus_message_iter_init(reply, &iter) == FALSE)
		goto done;

	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		DBusMessageIter variant;

		dbus_message_iter_recurse(&iter, &variant);

		if (data->function != NULL)
			data->function(NULL, &variant, data->user_data);
	}
done:
	dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

int supplicant_dbus_property_get(const char *path, const char *interface,
				const char *method,
				supplicant_dbus_property_function function,
							void *user_data)
{
	struct property_get_data *data;
	DBusMessage *message;
	DBusPendingCall *call;

	if (connection == NULL)
		return -EINVAL;

	if (path == NULL || interface == NULL || method == NULL)
		return -EINVAL;

	data = dbus_malloc0(sizeof(*data));
	if (data == NULL)
		return -ENOMEM;

	message = dbus_message_new_method_call(SUPPLICANT_SERVICE, path,
					DBUS_INTERFACE_PROPERTIES, "Get");

	if (message == NULL) {
		dbus_free(data);
		return -ENOMEM;
	}

	dbus_message_set_auto_start(message, FALSE);

	dbus_message_append_args(message, DBUS_TYPE_STRING, &interface,
					DBUS_TYPE_STRING, &method, NULL);

	if (dbus_connection_send_with_reply(connection, message,
						&call, TIMEOUT) == FALSE) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	if (call == NULL) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	data->function = function;
	data->user_data = user_data;

	dbus_pending_call_set_notify(call, property_get_reply,
							data, dbus_free);

	dbus_message_unref(message);

	return 0;
}

struct property_set_data {
	supplicant_dbus_result_function function;
	void *user_data;
};

static void property_set_reply(DBusPendingCall *call, void *user_data)
{
	struct property_set_data *data = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;
	const char *error;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		error = dbus_message_get_error_name(reply);
	else
		error = NULL;

	dbus_message_iter_init(reply, &iter);

	if (data->function != NULL)
		data->function(error, &iter, data->user_data);

	dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

int supplicant_dbus_property_set(const char *path, const char *interface,
				const char *key, const char *signature,
				supplicant_dbus_setup_function setup,
				supplicant_dbus_result_function function,
							void *user_data)
{
	struct property_set_data *data;
	DBusMessage *message;
	DBusMessageIter iter, value;
	DBusPendingCall *call;

	if (connection == NULL)
		return -EINVAL;

	if (path == NULL || interface == NULL)
		return -EINVAL;

	if (key == NULL || signature == NULL || setup == NULL)
		return -EINVAL;

	data = dbus_malloc0(sizeof(*data));
	if (data == NULL)
		return -ENOMEM;

	message = dbus_message_new_method_call(SUPPLICANT_SERVICE, path,
					DBUS_INTERFACE_PROPERTIES, "Set");
	if (message == NULL) {
		dbus_free(data);
		return -ENOMEM;
	}

	dbus_message_set_auto_start(message, FALSE);

	dbus_message_iter_init_append(message, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
							signature, &value);
	setup(&value, user_data);
	dbus_message_iter_close_container(&iter, &value);

	if (dbus_connection_send_with_reply(connection, message,
						&call, TIMEOUT) == FALSE) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	if (call == NULL) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	data->function = function;
	data->user_data = user_data;

	dbus_pending_call_set_notify(call, property_set_reply,
							data, dbus_free);

	dbus_message_unref(message);

	return 0;
}

struct method_call_data {
	supplicant_dbus_result_function function;
	void *user_data;
};

static void method_call_reply(DBusPendingCall *call, void *user_data)
{
	struct method_call_data *data = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;
	const char *error;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		error = dbus_message_get_error_name(reply);
	else
		error = NULL;

	dbus_message_iter_init(reply, &iter);

	if (data->function != NULL)
		data->function(error, &iter, data->user_data);

	dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

int supplicant_dbus_method_call(const char *path,
				const char *interface, const char *method,
				supplicant_dbus_setup_function setup,
				supplicant_dbus_result_function function,
							void *user_data)
{
	struct method_call_data *data;
	DBusMessage *message;
	DBusMessageIter iter;
	DBusPendingCall *call;

	if (connection == NULL)
		return -EINVAL;

	if (path == NULL || interface == NULL || method == NULL)
		return -EINVAL;

	data = dbus_malloc0(sizeof(*data));
	if (data == NULL)
		return -ENOMEM;

	message = dbus_message_new_method_call(SUPPLICANT_SERVICE, path,
							interface, method);
	if (message == NULL) {
		dbus_free(data);
		return -ENOMEM;
	}

	dbus_message_set_auto_start(message, FALSE);

	dbus_message_iter_init_append(message, &iter);
	if (setup != NULL)
		setup(&iter, user_data);

	if (dbus_connection_send_with_reply(connection, message,
						&call, TIMEOUT) == FALSE) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	if (call == NULL) {
		dbus_message_unref(message);
		dbus_free(data);
		return -EIO;
	}

	data->function = function;
	data->user_data = user_data;

	dbus_pending_call_set_notify(call, method_call_reply,
							data, dbus_free);

	dbus_message_unref(message);

	return 0;
}

void supplicant_dbus_property_append_basic(DBusMessageIter *iter,
					const char *key, int type, void *val)
{
	DBusMessageIter value;
	const char *signature;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &key);

	switch (type) {
	case DBUS_TYPE_BOOLEAN:
		signature = DBUS_TYPE_BOOLEAN_AS_STRING;
		break;
	case DBUS_TYPE_STRING:
		signature = DBUS_TYPE_STRING_AS_STRING;
		break;
	case DBUS_TYPE_BYTE:
		signature = DBUS_TYPE_BYTE_AS_STRING;
		break;
	case DBUS_TYPE_UINT16:
		signature = DBUS_TYPE_UINT16_AS_STRING;
		break;
	case DBUS_TYPE_INT16:
		signature = DBUS_TYPE_INT16_AS_STRING;
		break;
	case DBUS_TYPE_UINT32:
		signature = DBUS_TYPE_UINT32_AS_STRING;
		break;
	case DBUS_TYPE_INT32:
		signature = DBUS_TYPE_INT32_AS_STRING;
		break;
	case DBUS_TYPE_OBJECT_PATH:
		signature = DBUS_TYPE_OBJECT_PATH_AS_STRING;
		break;
	default:
		signature = DBUS_TYPE_VARIANT_AS_STRING;
		break;
	}

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
							signature, &value);
	dbus_message_iter_append_basic(&value, type, val);
	dbus_message_iter_close_container(iter, &value);
}

void supplicant_dbus_property_append_fixed_array(DBusMessageIter *iter,
				const char *key, int type, void *val, int len)
{
	DBusMessageIter value, array;
	const char *variant_sig, *array_sig;

	switch (type) {
	case DBUS_TYPE_BYTE:
		variant_sig = DBUS_TYPE_ARRAY_AS_STRING
					DBUS_TYPE_BYTE_AS_STRING;
		array_sig = DBUS_TYPE_BYTE_AS_STRING;
		break;
	default:
		return;
	}

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
							variant_sig, &value);

	dbus_message_iter_open_container(&value, DBUS_TYPE_ARRAY,
							array_sig, &array);
	dbus_message_iter_append_fixed_array(&array, type, val, len);
	dbus_message_iter_close_container(&value, &array);

	dbus_message_iter_close_container(iter, &value);
}

void supplicant_dbus_property_append_array(DBusMessageIter *iter,
				const char *key, int type,
				supplicant_dbus_array_function function,
				void *user_data)
{
	DBusMessageIter value, array;
	const char *variant_sig, *array_sig;

	switch (type) {
	case DBUS_TYPE_STRING:
		variant_sig = DBUS_TYPE_ARRAY_AS_STRING
				DBUS_TYPE_ARRAY_AS_STRING
				DBUS_TYPE_BYTE_AS_STRING;
		array_sig = DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING;
		break;
	default:
		return;
	}

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
							variant_sig, &value);

	dbus_message_iter_open_container(&value, DBUS_TYPE_ARRAY,
							array_sig, &array);
	if (function)
		function(&array, user_data);

	dbus_message_iter_close_container(&value, &array);

	dbus_message_iter_close_container(iter, &value);
}
