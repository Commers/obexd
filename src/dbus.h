/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2007-2010  Nokia Corporation
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

#include <dbus/dbus.h>

#define OPENOBEX_SERVICE  "org.openobex"

#define OPENOBEX_MANAGER_PATH		"/"
#define OPENOBEX_MANAGER_INTERFACE	OPENOBEX_SERVICE ".Manager"
#define ERROR_INTERFACE			OPENOBEX_SERVICE ".Error"


void emit_transfer_progress(guint32 id, guint32 total, guint32 transfered);

void register_record(struct server *server, gpointer user_data);

gint request_service_authorization(struct server *server, GIOChannel *io,
					const char *address);

void manager_register_session(struct OBEX_session *os);
void manager_unregister_session(struct OBEX_session *os);
void manager_register_transfer(struct OBEX_session *os);
void manager_unregister_transfer(struct OBEX_session *os);
void manager_emit_transfer_started(struct OBEX_session *os);
void manager_emit_transfer_progress(struct OBEX_session *os);
void manager_emit_transfer_completed(struct OBEX_session *os);
int manager_request_authorization(struct OBEX_session *os, gint32 time,
		gchar **new_folder, gchar **new_name);

DBusConnection *obex_dbus_get_connection(void);
