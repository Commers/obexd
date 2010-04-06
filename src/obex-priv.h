/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2007-2010  Nokia Corporation
 *  Copyright (C) 2007-2010  Marcel Holtmann <marcel@holtmann.org>
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

struct obex_session {
	GIOChannel	*io;
	guint32		cid;
	guint16		tx_mtu;
	guint16		rx_mtu;
	guint8		cmd;
	gchar		*name;
	gchar		*type;
	time_t		time;
	gchar		*current_folder;
	guint8		*buf;
	gint32		offset;
	gint32		size;
	gpointer	object;
	gboolean	aborted;
	struct obex_service_driver *service;
	struct server *server;
	gboolean	checked;
	obex_t		*obex;
	obex_object_t	*obj;
	struct obex_mime_type_driver *driver;
	gboolean	finished;
};
