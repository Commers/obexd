/*
 *
 *  OBEX Client
 *
 *  Copyright (C) 2007-2008  Intel Corporation
 *  Copyright (C) 2007-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
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

#include <glib.h>
#include <gdbus.h>

#include "session.h"
#include "pbap.h"

#define ERROR_INF PBAP_INTERFACE ".Error"

static gchar *build_phonebook_path(const char *location, const char *item)
{
	gchar *path = NULL, *tmp, *tmp1;

	if (!g_ascii_strcasecmp(location, "INT") ||
			!g_ascii_strcasecmp(location, "INTERNAL"))
		path = g_strdup("telecom");
	else if (!g_ascii_strncasecmp(location, "SIM", 3)) {
		if (strlen(location) == 3)
			tmp = g_strdup("SIM1");
		else
			tmp = g_ascii_strup(location, 4);

		path = g_build_filename(tmp, "telecom", NULL);
		g_free(tmp);
	} else
		return NULL;

	if (!g_ascii_strcasecmp(item, "PB") ||
		!g_ascii_strcasecmp(item, "ICH") ||
		!g_ascii_strcasecmp(item, "OCH") ||
		!g_ascii_strcasecmp(item, "MCH") ||
		!g_ascii_strcasecmp(item, "CCH")) {
		tmp = path;
		tmp1 = g_ascii_strdown(item, -1);
		path = g_build_filename(tmp, tmp1, NULL);
		g_free(tmp);
		g_free(tmp1);
	} else {
		g_free(path);
		return NULL;
	}

	return path;
}

/* should only be called inside pbap_set_path */
static void pbap_reset_path(struct session_data *session)
{
	int err = 0;
	char **paths = NULL, **item;
	struct pbap_data *pbapdata = session->pbapdata;

	if (!pbapdata->path)
		return;

	gw_obex_chdir(session->obex, "", &err);

	paths = g_strsplit(pbapdata->path, "/", 3);

	for (item = paths; *item; item++)
		gw_obex_chdir(session->obex, *item, &err);

	g_strfreev(paths);
}

static gint pbap_set_path(struct session_data *session, const char *path)
{
	int err = 0;
	char **paths = NULL, **item;
	struct pbap_data *pbapdata = session->pbapdata;

	if (!path)
		return OBEX_RSP_BAD_REQUEST;

	if (pbapdata->path != NULL && 	g_str_equal(pbapdata->path, path))
		return 0;

	if (gw_obex_chdir(session->obex, "", &err) == FALSE) {
		if (err == OBEX_RSP_NOT_IMPLEMENTED)
			goto done;
		goto fail;
	}

	paths = g_strsplit(path, "/", 3);
	for (item = paths; *item; item++) {
		if (gw_obex_chdir(session->obex, *item, &err) == FALSE) {
			/* we need to reset the path to the saved one on fail*/
			pbap_reset_path(session);
			goto fail;
		}
	}

	g_strfreev(paths);

done:
	g_free(pbapdata->path);
	pbapdata->path = g_strdup(path);
	return 0;

fail:
	if (paths)
		g_strfreev(paths);

	return err;
}

static DBusMessage *pbap_select(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	const char *item, *location;
	char *path = NULL;
	int err = 0;

	if (dbus_message_get_args(message, NULL,
			DBUS_TYPE_STRING, &location,
			DBUS_TYPE_STRING, &item,
			DBUS_TYPE_INVALID) == FALSE)
		return g_dbus_create_error(message,
				ERROR_INF ".InvalidArguments", NULL);

	path = build_phonebook_path(location, item);
	if (!path)
		return g_dbus_create_error(message,
				ERROR_INF ".InvalidArguments", "InvalidPhonebook");

	err = pbap_set_path(session, path);
	g_free(path);
	if (err)
		return g_dbus_create_error(message,
				ERROR_INF ".Failed",
				OBEX_ResponseToString(err));

	return dbus_message_new_method_return(message);
}

static GDBusMethodTable pbap_methods[] = {
	{ "Select",	"ss",	"",	pbap_select },
	{ }
};

gboolean pbap_register_interface(DBusConnection *connection, const char *path,
				void *user_data, GDBusDestroyFunction destroy)
{
	struct session_data *session = user_data;

	session->pbapdata = g_try_malloc0(sizeof(struct pbap_data));
	if (!session->pbapdata)
		return FALSE;

	return g_dbus_register_interface(connection, path, PBAP_INTERFACE,
				pbap_methods, NULL, NULL, user_data, destroy);
}

void pbap_unregister_interface(DBusConnection *connection, const char *path,
				void *user_data)
{
	struct session_data *session = user_data;

	g_dbus_unregister_interface(connection, path, PBAP_INTERFACE);
	if (session->pbapdata)
		g_free(session->pbapdata);
}
