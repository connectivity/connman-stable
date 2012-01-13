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
#include <stdlib.h>

#include <gweb/gweb.h>

#include "connman.h"

#define STATUS_URL_IPV4  "http://ipv4.connman.net/online/status.html"
#define STATUS_URL_IPV6  "http://ipv6.connman.net/online/status.html"

struct connman_wispr_message {
	gboolean has_error;
	const char *current_element;
	int message_type;
	int response_code;
	char *login_url;
	char *abort_login_url;
	char *logoff_url;
	char *access_procedure;
	char *access_location;
	char *location_name;
};

enum connman_wispr_result {
	CONNMAN_WISPR_RESULT_UNKNOWN = 0,
	CONNMAN_WISPR_RESULT_LOGIN   = 1,
	CONNMAN_WISPR_RESULT_ONLINE  = 2,
	CONNMAN_WISPR_RESULT_FAILED  = 3,
};

struct connman_wispr_portal_context {
	struct connman_service *service;
	enum connman_ipconfig_type type;

	/* Portal/WISPr common */
	GWeb *web;
	unsigned int token;
	guint request_id;

	const char *status_url;

	/* WISPr specific */
	GWebParser *wispr_parser;
	struct connman_wispr_message wispr_msg;

	char *wispr_username;
	char *wispr_password;
	char *wispr_formdata;

	enum connman_wispr_result wispr_result;
};

struct connman_wispr_portal {
	struct connman_wispr_portal_context *ipv4_context;
	struct connman_wispr_portal_context *ipv6_context;
};

static gboolean wispr_portal_web_result(GWebResult *result, gpointer user_data);

static GHashTable *wispr_portal_list = NULL;

static void connman_wispr_message_init(struct connman_wispr_message *msg)
{
	DBG("");

	msg->has_error = FALSE;
	msg->current_element = NULL;

	msg->message_type = -1;
	msg->response_code = -1;

	g_free(msg->login_url);
	msg->login_url = NULL;

	g_free(msg->abort_login_url);
	msg->abort_login_url = NULL;

	g_free(msg->logoff_url);
	msg->logoff_url = NULL;

	g_free(msg->access_procedure);
	msg->access_procedure = NULL;

	g_free(msg->access_location);
	msg->access_location = NULL;

	g_free(msg->location_name);
	msg->location_name = NULL;
}

static void free_connman_wispr_portal_context(struct connman_wispr_portal_context *wp_context)
{
	DBG("");

	if (wp_context == NULL)
		return;

	connman_service_unref(wp_context->service);

	if (wp_context->token > 0)
		connman_proxy_lookup_cancel(wp_context->token);

	if (wp_context->request_id > 0)
		g_web_cancel_request(wp_context->web, wp_context->request_id);

	g_web_unref(wp_context->web);

	g_web_parser_unref(wp_context->wispr_parser);
	connman_wispr_message_init(&wp_context->wispr_msg);

	g_free(wp_context->wispr_username);
	g_free(wp_context->wispr_password);
	g_free(wp_context->wispr_formdata);

	g_free(wp_context);
}

static void free_connman_wispr_portal(gpointer data)
{
	struct connman_wispr_portal *wispr_portal = data;

	DBG("");

	if (wispr_portal == NULL)
		return;

	free_connman_wispr_portal_context(wispr_portal->ipv4_context);
	free_connman_wispr_portal_context(wispr_portal->ipv6_context);

	g_free(wispr_portal);
}

static const char *message_type_to_string(int message_type)
{
	switch (message_type) {
	case 100:
		return "Initial redirect message";
	case 110:
		return "Proxy notification";
	case 120:
		return "Authentication notification";
	case 130:
		return "Logoff notification";
	case 140:
		return "Response to Authentication Poll";
	case 150:
		return "Response to Abort Login";
	}

	return NULL;
}

static const char *response_code_to_string(int response_code)
{
	switch (response_code) {
	case 0:
		return "No error";
	case 50:
		return "Login succeeded";
	case 100:
		return "Login failed";
	case 102:
		return "RADIUS server error/timeout";
	case 105:
		return "RADIUS server not enabled";
	case 150:
		return "Logoff succeeded";
	case 151:
		return "Login aborted";
	case 200:
		return "Proxy detection/repeat operation";
	case 201:
		return "Authentication pending";
	case 255:
		return "Access gateway internal error";
	}

	return NULL;
}

static struct {
	const char *str;
	enum {
		WISPR_ELEMENT_NONE              = 0,
		WISPR_ELEMENT_ACCESS_PROCEDURE  = 1,
		WISPR_ELEMENT_ACCESS_LOCATION   = 2,
		WISPR_ELEMENT_LOCATION_NAME     = 3,
		WISPR_ELEMENT_LOGIN_URL         = 4,
		WISPR_ELEMENT_ABORT_LOGIN_URL   = 5,
		WISPR_ELEMENT_MESSAGE_TYPE      = 6,
		WISPR_ELEMENT_RESPONSE_CODE     = 7,
		WISPR_ELEMENT_NEXT_URL          = 8,
		WISPR_ELEMENT_DELAY             = 9,
		WISPR_ELEMENT_REPLY_MESSAGE     = 10,
		WISPR_ELEMENT_LOGIN_RESULTS_URL = 11,
		WISPR_ELEMENT_LOGOFF_URL        = 12,
	} element;
} wispr_element_map[] = {
	{ "AccessProcedure",	WISPR_ELEMENT_ACCESS_PROCEDURE	},
	{ "AccessLocation",	WISPR_ELEMENT_ACCESS_LOCATION	},
	{ "LocationName",	WISPR_ELEMENT_LOCATION_NAME	},
	{ "LoginURL",		WISPR_ELEMENT_LOGIN_URL		},
	{ "AbortLoginURL",	WISPR_ELEMENT_ABORT_LOGIN_URL	},
	{ "MessageType",	WISPR_ELEMENT_MESSAGE_TYPE	},
	{ "ResponseCode",	WISPR_ELEMENT_RESPONSE_CODE	},
	{ "NextURL",		WISPR_ELEMENT_NEXT_URL		},
	{ "Delay",		WISPR_ELEMENT_DELAY		},
	{ "ReplyMessage",	WISPR_ELEMENT_REPLY_MESSAGE	},
	{ "LoginResultsURL",	WISPR_ELEMENT_LOGIN_RESULTS_URL	},
	{ "LogoffURL",		WISPR_ELEMENT_LOGOFF_URL	},
	{ NULL,			WISPR_ELEMENT_NONE		},
};

static void xml_wispr_start_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **attribute_names,
					const gchar **attribute_values,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;

	msg->current_element = element_name;
}

static void xml_wispr_end_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;

	msg->current_element = NULL;
}

static void xml_wispr_text_handler(GMarkupParseContext *context,
					const gchar *text, gsize text_len,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;
	int i;

	if (msg->current_element == NULL)
		return;

	for (i = 0; wispr_element_map[i].str; i++) {
		if (g_str_equal(wispr_element_map[i].str,
					msg->current_element) == FALSE)
			continue;

		switch (wispr_element_map[i].element) {
		case WISPR_ELEMENT_NONE:
		case WISPR_ELEMENT_ACCESS_PROCEDURE:
			g_free(msg->access_procedure);
			msg->access_procedure = g_strdup(text);
			break;
		case WISPR_ELEMENT_ACCESS_LOCATION:
			g_free(msg->access_location);
			msg->access_location = g_strdup(text);
			break;
		case WISPR_ELEMENT_LOCATION_NAME:
			g_free(msg->location_name);
			msg->location_name = g_strdup(text);
			break;
		case WISPR_ELEMENT_LOGIN_URL:
			g_free(msg->login_url);
			msg->login_url = g_strdup(text);
			break;
		case WISPR_ELEMENT_ABORT_LOGIN_URL:
			g_free(msg->abort_login_url);
			msg->abort_login_url = g_strdup(text);
			break;
		case WISPR_ELEMENT_MESSAGE_TYPE:
			msg->message_type = atoi(text);
			break;
		case WISPR_ELEMENT_RESPONSE_CODE:
			msg->response_code = atoi(text);
			break;
		case WISPR_ELEMENT_NEXT_URL:
		case WISPR_ELEMENT_DELAY:
		case WISPR_ELEMENT_REPLY_MESSAGE:
		case WISPR_ELEMENT_LOGIN_RESULTS_URL:
			break;
		case WISPR_ELEMENT_LOGOFF_URL:
			g_free(msg->logoff_url);
			msg->logoff_url = g_strdup(text);
			break;
		}
	}
}

static void xml_wispr_error_handler(GMarkupParseContext *context,
					GError *error, gpointer user_data)
{
	struct connman_wispr_message *msg = user_data;

	msg->has_error = TRUE;
}

static const GMarkupParser xml_wispr_parser_handlers = {
	xml_wispr_start_element_handler,
	xml_wispr_end_element_handler,
	xml_wispr_text_handler,
	NULL,
	xml_wispr_error_handler,
};

static void xml_wispr_parser_callback(const char *str, gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	GMarkupParseContext *parser_context = NULL;
	gboolean result;

	DBG("");

	parser_context = g_markup_parse_context_new(&xml_wispr_parser_handlers,
					G_MARKUP_TREAT_CDATA_AS_TEXT,
					&(wp_context->wispr_msg), NULL);

	result = g_markup_parse_context_parse(parser_context,
					str, strlen(str), NULL);
	if (result == TRUE)
		result = g_markup_parse_context_end_parse(parser_context, NULL);

	g_markup_parse_context_free(parser_context);
}

static void web_debug(const char *str, void *data)
{
	connman_info("%s: %s\n", (const char *) data, str);
}

static void wispr_portal_error(struct connman_wispr_portal_context *wp_context)
{
	DBG("Failed to proceed wispr/portal web request");

	wp_context->wispr_result = CONNMAN_WISPR_RESULT_FAILED;
}

static void portal_manage_status(GWebResult *result,
			struct connman_wispr_portal_context *wp_context)
{
	const char *str = NULL;

	DBG("");

	/* We currently don't do anything with this info */
	if (g_web_result_get_header(result, "X-ConnMan-Client-IP",
				&str) == TRUE)
		connman_info("Client-IP: %s", str);

	if (g_web_result_get_header(result, "X-ConnMan-Client-Country",
				&str) == TRUE)
		connman_info("Client-Country: %s", str);

	if (g_web_result_get_header(result, "X-ConnMan-Client-Region",
				&str) == TRUE)
		connman_info("Client-Region: %s", str);

	__connman_service_ipconfig_indicate_state(wp_context->service,
						CONNMAN_SERVICE_STATE_ONLINE,
						wp_context->type);
}

static void wispr_portal_request_portal(struct connman_wispr_portal_context *wp_context)
{
	DBG("");

	wp_context->request_id = g_web_request_get(wp_context->web,
					wp_context->status_url,
					wispr_portal_web_result, wp_context);

	if (wp_context->request_id == 0)
		wispr_portal_error(wp_context);
}

static gboolean wispr_input(const guint8 **data, gsize *length,
						gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	GString *buf;
	gsize count;

	DBG("");

	buf = g_string_sized_new(100);

	g_string_append(buf, "button=Login&UserName=");
	g_string_append_uri_escaped(buf, wp_context->wispr_username,
								NULL, FALSE);
	g_string_append(buf, "&Password=");
	g_string_append_uri_escaped(buf, wp_context->wispr_password,
								NULL, FALSE);
	g_string_append(buf, "&FNAME=0&OriginatingServer=");
	g_string_append_uri_escaped(buf, wp_context->status_url, NULL, FALSE);

	count = buf->len;

	g_free(wp_context->wispr_formdata);
	wp_context->wispr_formdata = g_string_free(buf, FALSE);

	*data = (guint8 *) wp_context->wispr_formdata;
	*length = count;

	return FALSE;
}

static void wispr_portal_request_wispr_login(struct connman_service *service,
				const char *username, const char *password,
				void *user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;

	DBG("");

	g_free(wp_context->wispr_username);
	wp_context->wispr_username = g_strdup(username);

	g_free(wp_context->wispr_password);
	wp_context->wispr_password = g_strdup(password);

	wp_context->request_id = g_web_request_post(wp_context->web,
					wp_context->wispr_msg.login_url,
					"application/x-www-form-urlencoded",
					wispr_input, wispr_portal_web_result,
					wp_context);

	connman_wispr_message_init(&wp_context->wispr_msg);
}

static gboolean wispr_manage_message(GWebResult *result,
			struct connman_wispr_portal_context *wp_context)
{
	DBG("Message type: %s (%d)",
		message_type_to_string(wp_context->wispr_msg.message_type),
					wp_context->wispr_msg.message_type);
	DBG("Response code: %s (%d)",
		response_code_to_string(wp_context->wispr_msg.response_code),
					wp_context->wispr_msg.response_code);

	if (wp_context->wispr_msg.access_procedure != NULL)
		DBG("Access procedure: %s",
			wp_context->wispr_msg.access_procedure);
	if (wp_context->wispr_msg.access_location != NULL)
		DBG("Access location: %s",
			wp_context->wispr_msg.access_location);
	if (wp_context->wispr_msg.location_name != NULL)
		DBG("Location name: %s",
			wp_context->wispr_msg.location_name);
	if (wp_context->wispr_msg.login_url != NULL)
		DBG("Login URL: %s", wp_context->wispr_msg.login_url);
	if (wp_context->wispr_msg.abort_login_url != NULL)
		DBG("Abort login URL: %s",
			wp_context->wispr_msg.abort_login_url);
	if (wp_context->wispr_msg.logoff_url != NULL)
		DBG("Logoff URL: %s", wp_context->wispr_msg.logoff_url);

	switch (wp_context->wispr_msg.message_type) {
	case 100:
		DBG("Login required");

		wp_context->wispr_result = CONNMAN_WISPR_RESULT_LOGIN;

		__connman_service_request_login(wp_context->service);

		if (__connman_agent_request_login_input(wp_context->service,
					wispr_portal_request_wispr_login,
					wp_context) != -EIO)
			wispr_portal_error(wp_context);

		break;
	case 120: /* Falling down */
	case 140:
		if (wp_context->wispr_msg.response_code == 50) {
			wp_context->wispr_result = CONNMAN_WISPR_RESULT_ONLINE;

			g_free(wp_context->wispr_username);
			wp_context->wispr_username = NULL;

			g_free(wp_context->wispr_password);
			wp_context->wispr_password = NULL;

			g_free(wp_context->wispr_formdata);
			wp_context->wispr_formdata = NULL;

			wispr_portal_request_portal(wp_context);

			return TRUE;
		} else
			wispr_portal_error(wp_context);

		break;
	default:
		break;
	}

	return FALSE;
}

static gboolean wispr_portal_web_result(GWebResult *result, gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	const char *redirect = NULL;
	const guint8 *chunk = NULL;
	const char *str = NULL;
	guint16 status;
	gsize length;

	DBG("");

	if (wp_context->request_id == 0)
		return FALSE;

	if (wp_context->wispr_result != CONNMAN_WISPR_RESULT_ONLINE) {
		g_web_result_get_chunk(result, &chunk, &length);

		if (length > 0) {
			g_web_parser_feed_data(wp_context->wispr_parser,
								chunk, length);
			return TRUE;
		}

		g_web_parser_end_data(wp_context->wispr_parser);

		if (wp_context->wispr_msg.message_type >= 0) {
			if (wispr_manage_message(result, wp_context) == TRUE)
				goto done;
		}
	}

	status = g_web_result_get_status(result);

	DBG("status: %03u", status);

	switch (status) {
	case 200:
		if (wp_context->wispr_msg.message_type >= 0)
			break;

		if (g_web_result_get_header(result, "X-ConnMan-Status",
								&str) == TRUE)
			portal_manage_status(result, wp_context);
		else
			__connman_service_request_login(wp_context->service);

		break;
	case 302:
		if (g_web_result_get_header(result, "Location",
						&redirect) == FALSE)
			break;

		DBG("Redirect URL: %s", redirect);

		wp_context->request_id = g_web_request_get(wp_context->web,
				redirect, wispr_portal_web_result, wp_context);

		goto done;
	case 404:
		wispr_portal_error(wp_context);

		break;
	default:
		break;
	}

	wp_context->request_id = 0;
done:
	wp_context->wispr_msg.message_type = -1;
	return FALSE;
}

static void proxy_callback(const char *proxy, void *user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;

	DBG("proxy %s", proxy);

	wp_context->token = 0;

	if (proxy == NULL)
		proxy = getenv("http_proxy");

	if (getenv("CONNMAN_WEB_DEBUG"))
		g_web_set_debug(wp_context->web, web_debug, "WEB");

	if (proxy != NULL && g_strcmp0(proxy, "DIRECT") != 0)
		g_web_set_proxy(wp_context->web, proxy);

	g_web_set_accept(wp_context->web, NULL);
	g_web_set_user_agent(wp_context->web, "ConnMan/%s wispr", VERSION);
	g_web_set_close_connection(wp_context->web, TRUE);

	connman_wispr_message_init(&wp_context->wispr_msg);

	wp_context->wispr_parser = g_web_parser_new(
					"<WISPAccessGatewayParam",
					"WISPAccessGatewayParam>",
					xml_wispr_parser_callback, wp_context);

	wispr_portal_request_portal(wp_context);
}

static int wispr_portal_detect(struct connman_wispr_portal_context *wp_context)
{
	enum connman_service_type service_type;
	char *interface = NULL;
	int if_index;
	int err = 0;

	DBG("wispr/portal context %p", wp_context);
	DBG("service %p", wp_context->service);

	service_type = connman_service_get_type(wp_context->service);

	switch (service_type) {
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIFI:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		break;
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return -EOPNOTSUPP;
	}

	interface = connman_service_get_interface(wp_context->service);
	if (interface == NULL)
		return -EINVAL;

	DBG("interface %s", interface);

	if_index = connman_inet_ifindex(interface);
	if (if_index < 0)
		return -EINVAL;

	wp_context->web = g_web_new(if_index);
	if (wp_context->web == NULL) {
		err = -ENOMEM;
		goto done;
	}

	if (wp_context->type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		g_web_set_address_family(wp_context->web, AF_INET);
		wp_context->status_url = STATUS_URL_IPV4;
	} else {
		g_web_set_address_family(wp_context->web, AF_INET6);
		wp_context->status_url = STATUS_URL_IPV6;
	}

	wp_context->token = connman_proxy_lookup(interface,
					wp_context->status_url,
					wp_context->service,
					proxy_callback, wp_context);
	if (wp_context->token == 0)
		err = -EINVAL;

done:
	g_free(interface);
	return err;
}

int __connman_wispr_start(struct connman_service *service,
					enum connman_ipconfig_type type)
{
	struct connman_wispr_portal_context *wp_context = NULL;
	struct connman_wispr_portal *wispr_portal = NULL;
	int index;

	DBG("service %p", service);

	if (wispr_portal_list == NULL)
		return -EINVAL;

	index = __connman_service_get_index(service);
	if (index < 0)
		return -EINVAL;

	wispr_portal = g_hash_table_lookup(wispr_portal_list,
					GINT_TO_POINTER(index));
	if (wispr_portal == NULL) {
		wispr_portal = g_try_new0(struct connman_wispr_portal, 1);
		if (wispr_portal == NULL)
			return -ENOMEM;

		g_hash_table_replace(wispr_portal_list,
					GINT_TO_POINTER(index), wispr_portal);
	}

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		wp_context = wispr_portal->ipv4_context;
	else if (type == CONNMAN_IPCONFIG_TYPE_IPV6)
		wp_context = wispr_portal->ipv6_context;
	else
		return -EINVAL;

	/* If there is already an existing context, we wipe it */
	if (wp_context != NULL)
		free_connman_wispr_portal_context(wp_context);

	wp_context = g_try_new0(struct connman_wispr_portal_context, 1);
	if (wp_context == NULL)
		return -ENOMEM;

	connman_service_ref(service);

	wp_context->service = service;
	wp_context->type = type;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		wispr_portal->ipv4_context = wp_context;
	else
		wispr_portal->ipv6_context = wp_context;

	return wispr_portal_detect(wp_context);
}

void __connman_wispr_stop(struct connman_service *service)
{
	int index;

	DBG("service %p", service);

	if (wispr_portal_list == NULL)
		return;

	index = __connman_service_get_index(service);
	if (index < 0)
		return;

	g_hash_table_remove(wispr_portal_list, GINT_TO_POINTER(index));
}

int __connman_wispr_init(void)
{
	DBG("");

	wispr_portal_list = g_hash_table_new_full(g_direct_hash,
						g_direct_equal, NULL,
						free_connman_wispr_portal);

	return 0;
}

void __connman_wispr_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(wispr_portal_list);
	wispr_portal_list = NULL;
}
