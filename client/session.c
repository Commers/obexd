/*
 *
 *  OBEX Client
 *
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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>
#include <gdbus.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "session.h"

#define AGENT_INTERFACE  "org.openobex.Agent"

#define TRANSFER_INTERFACE  "org.openobex.Transfer"
#define TRANSFER_BASEPATH   "/org/openobex"

#define SESSION_INTERFACE  "org.openobex.Session"
#define SESSION_BASEPATH   "/org/openobex"

#define FTP_INTERFACE  "org.openobex.FileTransfer"

static guint64 counter = 0;

struct callback_data {
	struct session_data *session;
	sdp_session_t *sdp;
	session_callback_t func;
	void *data;
};

static struct session_data *session_ref(struct session_data *session)
{
	g_atomic_int_inc(&session->refcount);

	return session;
}

static void session_unref(struct session_data *session)
{
	if (g_atomic_int_dec_and_test(&session->refcount) == FALSE)
		return;

	if (session->agent_name != NULL) {
		DBusMessage *message;

		message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Release");

		dbus_message_set_no_reply(message, TRUE);

		g_dbus_send_message(session->conn, message);
	}

	if (session->pending != NULL)
		g_ptr_array_free(session->pending, TRUE);

	if (session->obex != NULL) {
		if (session->xfer != NULL) {
			gw_obex_xfer_close(session->xfer, NULL);
			gw_obex_xfer_free(session->xfer);
		}

		gw_obex_close(session->obex);
	}

	if (session->sock > 2)
		close(session->sock);

	if (session->conn) {
		if (session->path)
			g_dbus_unregister_interface(session->conn,
					session->path, TRANSFER_INTERFACE);

		dbus_connection_unref(session->conn);
	}

	g_free(session->path);
	g_free(session->name);
	g_free(session->filename);
	g_free(session->agent_name);
	g_free(session->agent_path);
	g_free(session);
}

static gboolean rfcomm_callback(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct callback_data *callback = user_data;
	struct session_data *session = callback->session;
	GwObex *obex;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR))
		goto done;

	fd = g_io_channel_unix_get_fd(io);

	obex = gw_obex_setup_fd(fd, session->target,
			session->target_len, NULL, NULL);

	callback->session->sock = fd;
	callback->session->obex = obex;

	callback->session->pending = g_ptr_array_new();

done:
	callback->func(callback->session, callback->data);

	session_unref(callback->session);

	g_free(callback);

	return FALSE;
}

static int rfcomm_connect(const bdaddr_t *src,
				const bdaddr_t *dst, uint8_t channel,
					GIOFunc function, gpointer user_data)
{
	GIOChannel *io;
	struct sockaddr_rc addr;
	int sk;

	sk = socket(PF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0)
		return -EIO;

	memset(&addr, 0, sizeof(addr));
	addr.rc_family = AF_BLUETOOTH;
	bacpy(&addr.rc_bdaddr, src);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sk);
		return -EIO;
	}

	io = g_io_channel_unix_new(sk);
	if (io == NULL) {
		close(sk);
		return -ENOMEM;
	}

	if (g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK,
						NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(io);
		close(sk);
		return -EPERM;
	}

	memset(&addr, 0, sizeof(addr));
	addr.rc_family = AF_BLUETOOTH;
	bacpy(&addr.rc_bdaddr, dst);
	addr.rc_channel = channel;

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		if (errno != EAGAIN && errno != EINPROGRESS) {
			g_io_channel_unref(io);
			close(sk);
			return -EIO;
		}
	}

	g_io_add_watch(io, G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							function, user_data);

	g_io_channel_unref(io);

	return 0;
}

static void search_callback(uint8_t type, uint16_t status,
			uint8_t *rsp, size_t size, void *user_data)
{
	struct callback_data *callback = user_data;
	int scanned, seqlen = 0, bytesleft = size;
	uint8_t dataType, channel = 0;

	if (status || type != SDP_SVC_SEARCH_ATTR_RSP)
		goto failed;

	scanned = sdp_extract_seqtype(rsp, bytesleft, &dataType, &seqlen);
	if (!scanned || !seqlen)
		goto failed;

	rsp += scanned;
	bytesleft -= scanned;
	do {
		sdp_record_t *rec;
		sdp_list_t *protos;
		int recsize, ch = -1;

		recsize = 0;
		rec = sdp_extract_pdu(rsp, bytesleft, &recsize);
		if (!rec)
			break;

		if (!recsize) {
			sdp_record_free(rec);
			break;
		}

		if (!sdp_get_access_protos(rec, &protos)) {
			ch = sdp_get_proto_port(protos, RFCOMM_UUID);
			sdp_list_foreach(protos,
					(sdp_list_func_t) sdp_list_free, NULL);
			sdp_list_free(protos, NULL);
			protos = NULL;
		}

		sdp_record_free(rec);

		if (ch > 0) {
			channel = ch;
			break;
		}

		scanned += recsize;
		rsp += recsize;
		bytesleft -= recsize;
	} while (scanned < size && bytesleft > 0);

	if (channel == 0)
		goto failed;

	if (rfcomm_connect(&callback->session->src, &callback->session->dst,
					channel, rfcomm_callback, callback) == 0) {
		sdp_close(callback->sdp);
		return;
	}

failed:
	sdp_close(callback->sdp);

	callback->func(callback->session, callback->data);
	session_unref(callback->session);
	g_free(callback);
}

static gboolean process_callback(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct callback_data *callback = user_data;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		return FALSE;

	if (sdp_process(callback->sdp) < 0)
		return FALSE;

	return TRUE;
}

static gboolean service_callback(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct callback_data *callback = user_data;
	sdp_list_t *search, *attrid;
	uint32_t range = 0x0000ffff;
	uuid_t uuid;

	if (cond & (G_IO_NVAL | G_IO_ERR))
		goto failed;

	if (sdp_set_notify(callback->sdp, search_callback, callback) < 0)
		goto failed;

	sdp_uuid16_create(&uuid, callback->session->uuid);

	search = sdp_list_append(NULL, &uuid);
	attrid = sdp_list_append(NULL, &range);

	if (sdp_service_search_attr_async(callback->sdp,
				search, SDP_ATTR_REQ_RANGE, attrid) < 0) {
		sdp_list_free(attrid, NULL);
		sdp_list_free(search, NULL);
		goto failed;
	}

	sdp_list_free(attrid, NULL);
	sdp_list_free(search, NULL);

	g_io_add_watch(io, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
						process_callback, callback);

	return FALSE;

failed:
	sdp_close(callback->sdp);

	callback->func(callback->session, callback->data);
	session_unref(callback->session);
	g_free(callback);
	return FALSE;
}

static sdp_session_t *service_connect(const bdaddr_t *src, const bdaddr_t *dst,
					GIOFunc function, gpointer user_data)
{
	sdp_session_t *sdp;
	GIOChannel *io;

	sdp = sdp_connect(src, dst, SDP_NON_BLOCKING);
	if (sdp == NULL)
		return NULL;

	io = g_io_channel_unix_new(sdp_get_socket(sdp));
	if (io == NULL) {
		sdp_close(sdp);
		return NULL;
	}

	g_io_add_watch(io, G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
							function, user_data);

	g_io_channel_unref(io);

	return sdp;
}

int session_create(const char *source,
			const char *destination, const char *target,
				session_callback_t function, void *user_data)
{
	struct session_data *session;
	struct callback_data *callback;
	int err;

	if (destination == NULL)
		return -EINVAL;

	session = g_try_malloc0(sizeof(*session));
	if (session == NULL)
		return -ENOMEM;

	session->refcount = 1;
	session->sock = -1;

	session->conn = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	if (session->conn == NULL) {
		session_unref(session);
		return -ENOMEM;
	}

	if (source == NULL)
		bacpy(&session->src, BDADDR_ANY);
	else
		str2ba(source, &session->src);

	str2ba(destination, &session->dst);

	if (target != NULL) {
		session->uuid = OBEX_FILETRANS_SVCLASS_ID;
		session->target = OBEX_FTP_UUID;
		session->target_len = OBEX_FTP_UUID_LEN;
	} else
		session->uuid = OBEX_OBJPUSH_SVCLASS_ID;

	callback = g_try_malloc0(sizeof(*callback));
	if (callback == NULL) {
		session_unref(session);
		return -ENOMEM;
	}

	callback->session = session;
	callback->func = function;
	callback->data = user_data;

	if (session->channel > 0) {
		err = rfcomm_connect(&session->src, &session->dst,
				session->channel, rfcomm_callback, callback);
	} else {
		callback->sdp = service_connect(&session->src, &session->dst,
						service_callback, callback);
		err = (callback->sdp == NULL) ? -ENOMEM : 0;
	}

	if (err < 0) {
		session_unref(session);
		g_free(callback);
		return -EINVAL;
	}

	return 0;
}

static void abort_transfer(struct session_data *session)
{
	DBusMessage *message;

	message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Complete");

	dbus_message_append_args(message,
			DBUS_TYPE_OBJECT_PATH, &session->path,
						DBUS_TYPE_INVALID);

	g_dbus_send_message(session->conn, message);

	gw_obex_xfer_abort(session->xfer, NULL);

	gw_obex_xfer_free(session->xfer);
	session->xfer = NULL;

	g_free(session->filename);
	session->filename = NULL;

	g_free(session->name);
	session->name = NULL;

	if (session->path) {
		g_dbus_unregister_interface(session->conn,
				session->path, TRANSFER_INTERFACE);
		g_free(session->path);
		session->path = NULL;
	}

	if (session->pending->len > 0) {
		gchar *filename;
		filename = g_ptr_array_index(session->pending, 0);
		g_ptr_array_remove(session->pending, filename);

		session_send(session, filename, g_path_get_basename(filename));
		g_free(filename);
	}

	session_unref(session);
}

int session_set_agent(struct session_data *session, const char *name,
							const char *path)
{
	if (session == NULL)
		return -EINVAL;

	if (session->agent_name != NULL || session->agent_path != NULL)
		return -EALREADY;

	session->agent_name = g_strdup(name);
	session->agent_path = g_strdup(path);

	return 0;
}

static void append_entry(DBusMessageIter *dict,
				const char *key, int type, void *val)
{
	DBusMessageIter entry, value;
	const char *signature;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
								NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	switch (type) {
	case DBUS_TYPE_STRING:
		signature = DBUS_TYPE_STRING_AS_STRING;
		break;
	case DBUS_TYPE_UINT64:
		signature = DBUS_TYPE_UINT64_AS_STRING;
		break;
	default:
		signature = DBUS_TYPE_VARIANT_AS_STRING;
		break;
	}

	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
							signature, &value);
	dbus_message_iter_append_basic(&value, type, val);
	dbus_message_iter_close_container(&entry, &value);

	dbus_message_iter_close_container(dict, &entry);
}

static DBusMessage *get_properties(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	DBusMessage *reply;
	DBusMessageIter iter, dict;

	reply = dbus_message_new_method_return(message);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	append_entry(&dict, "Name", DBUS_TYPE_STRING, &session->name);
	append_entry(&dict, "Size", DBUS_TYPE_UINT64, &session->size);
	append_entry(&dict, "Filename", DBUS_TYPE_STRING, &session->filename);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *transfer_cancel(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	const gchar *sender;
	DBusMessage *reply;

	sender = dbus_message_get_sender(message);
	if (g_str_equal(sender, session->agent_name) == FALSE)
		return g_dbus_create_error(message,
				"org.openobex.Error.NotAuthorized",
				"Not Authorized");

	reply = dbus_message_new_method_return(message);
	if (!reply)
		return NULL;

	abort_transfer(session);

	return reply;
}

static GDBusMethodTable transfer_methods[] = {
	{ "GetProperties", "", "a{sv}", get_properties },
	{ "Cancel", "", "", transfer_cancel },
	{ }
};

static void agent_disconnected(DBusConnection *connection, void *user_data)
{
	struct session_data *session = user_data;

	if (session->agent_name) {
		g_free(session->agent_name);
		session->agent_name = NULL;
	}

	if (session->agent_path) {
		g_free(session->agent_path);
		session->agent_path = NULL;
	}
}

static DBusMessage *assign_agent(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	const gchar *sender;
	gchar *path;

	if (dbus_message_get_args(message, NULL,
					DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID) == FALSE)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments",
				"Invalid arguments in method call");

	if (session->agent_path != NULL || session->agent_name != NULL)
		return g_dbus_create_error(message,
				"org.openobex.Error.AlreadyExists",
				"Already exists");

	sender = dbus_message_get_sender(message);

	session->agent_name = g_strdup(sender);
	session->agent_path = g_strdup(path);

	g_dbus_add_disconnect_watch(connection, sender,
				agent_disconnected, session, NULL);

	return dbus_message_new_method_return(message);
}

static DBusMessage *release_agent(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	const gchar *sender;
	gchar *path;

	if (dbus_message_get_args(message, NULL,
					DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID) == FALSE)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments",
				"Invalid arguments in method call");

	sender = dbus_message_get_sender(message);

	if (g_str_equal(sender, session->agent_name) == FALSE ||
				g_str_equal(path, session->agent_path) == FALSE)
		return g_dbus_create_error(message,
				"org.openobex.Error.NotAuthorized",
				"Not Authorized");

	g_free(session->agent_name);
	session->agent_name = NULL;

	g_free(session->agent_path);
	session->agent_path = NULL;

	return dbus_message_new_method_return(message);
}

static GDBusMethodTable session_methods[] = {
	{ "GetProperties",	"", "a{sv}",	get_properties	},
	{ "AssignAgent",	"o", "",	assign_agent	},
	{ "ReleaseAgent",	"o", "",	release_agent	},
	{ }
};

static DBusMessage *change_folder(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	struct session_data *session = user_data;
	const char *folder;
	int err;

	if (dbus_message_get_args(message, NULL,
				DBUS_TYPE_STRING, &folder,
				DBUS_TYPE_INVALID) == FALSE)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	if (gw_obex_chdir(session->obex, folder, &err) == FALSE) {
		return g_dbus_create_error(message,
				"org.openobex.Error.Failed",
				OBEX_ResponseToString(err));
	}

	return dbus_message_new_method_return(message);
}

static DBusMessage *create_folder(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *list_folder(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *get_file(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *put_file(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *copy_file(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *move_file(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static DBusMessage *delete(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	return dbus_message_new_method_return(message);
}

static GDBusMethodTable ftp_methods[] = {
	{ "ChangeFolder",	"s", "",	change_folder	},
	{ "CreateFolder",	"s", "",	create_folder	},
	{ "ListFolder",		"s", "aa{sv}",	list_folder	},
	{ "GetFile",		"ss", "",	get_file	},
	{ "PutFile",		"ss", "",	put_file	},
	{ "CopyFile",		"ss", "",	copy_file	},
	{ "MoveFile",		"ss", "",	move_file	},
	{ "Delete",		"s", "",	delete		},
	{ }
};

static void put_xfer_progress(GwObexXfer *xfer, gpointer user_data)
{
	struct session_data *session = user_data;
	DBusMessage *message;
	ssize_t len;
	gint written;

	len = read(session->fd, session->buffer + session->filled,
				sizeof(session->buffer) - session->filled);
	if (len <= 0)
		goto complete;

	if (gw_obex_xfer_write(xfer, session->buffer, session->filled + len,
						&written, NULL) == FALSE)
		goto complete;

	gw_obex_xfer_flush(xfer, NULL);

	session->filled = (session->filled + len) - written;

	memmove(session->buffer + written, session->buffer, session->filled);

	session->transferred += written;

	message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Progress");

	dbus_message_set_no_reply(message, TRUE);

	dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &session->path,
				DBUS_TYPE_UINT64, &session->transferred,
							DBUS_TYPE_INVALID);

	g_dbus_send_message(session->conn, message);

	return;

complete:
	message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Complete");

	dbus_message_append_args(message,
			DBUS_TYPE_OBJECT_PATH, &session->path,
						DBUS_TYPE_INVALID);

	g_dbus_send_message(session->conn, message);

	if (session->pending->len > 0) {
		gchar *filename;
		filename = g_ptr_array_index(session->pending, 0);
		g_ptr_array_remove(session->pending, filename);

		gw_obex_xfer_close(session->xfer, NULL);
		gw_obex_xfer_free(session->xfer);
		session->xfer = NULL;

		g_free(session->filename);
		session->filename = NULL;

		g_free(session->name);
		session->name = NULL;

		if (session->path) {
			g_dbus_unregister_interface(session->conn,
					session->path, TRANSFER_INTERFACE);
			g_free(session->path);
			session->path = NULL;
		}

		session_send(session, filename, g_path_get_basename(filename));
		g_free(filename);
	}

	session_unref(session);
}

int session_send(struct session_data *session, const char *filename,
				const char *targetname)
{
	GwObexXfer *xfer;
	DBusMessage *message;
	guint64 transferred = 0;
	struct stat st;
	int fd;

	if (session->obex == NULL)
		return -ENOTCONN;

	if (session->xfer != NULL) {
		g_ptr_array_add(session->pending, g_strdup(filename));
		return 0;
	}

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -EIO;

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -EIO;
	}

	session->fd = fd;
	session->size = st.st_size;
	session->transferred = 0;
	session->filename = g_strdup(filename);
	session->name = g_strdup(targetname);

	if (session->path == NULL) {
		session->path = g_strdup_printf("%s/transfer%ju",
					TRANSFER_BASEPATH, counter++);

	if (g_dbus_register_interface(session->conn, session->path,
					TRANSFER_INTERFACE,
					transfer_methods, NULL, NULL,
						session, NULL) == FALSE)
		return -EIO;

	session_ref(session);

	message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Request");

	dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &session->path,
							DBUS_TYPE_INVALID);

	g_dbus_send_message(session->conn, message);

	xfer = gw_obex_put_async(session->obex, session->name, NULL,
						session->size, -1, NULL);
	if (xfer == NULL)
		return -ENOTCONN;

	message = dbus_message_new_method_call(session->agent_name,
			session->agent_path, AGENT_INTERFACE, "Progress");

	dbus_message_set_no_reply(message, TRUE);

	dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &session->path,
						DBUS_TYPE_UINT64, &transferred,
							DBUS_TYPE_INVALID);

	g_dbus_send_message(session->conn, message);

	gw_obex_xfer_set_callback(xfer, put_xfer_progress, session);

	session->xfer = xfer;

	return 0;
}

static void get_xfer_progress(GwObexXfer *xfer, gpointer user_data)
{
	struct callback_data *callback = user_data;
	char buf[1024];
	gint len;

	if (gw_obex_xfer_read(xfer, buf, sizeof(buf), &len, NULL) == FALSE)
		goto complete;

	if (len == gw_obex_xfer_object_size(xfer))
		goto complete;

	gw_obex_xfer_flush(xfer, NULL);

	return;

complete:
	gw_obex_xfer_close(xfer, NULL);
	gw_obex_xfer_free(xfer);
	callback->session->xfer = NULL;

	callback->func(callback->session, callback->data);

	session_unref(callback->session);

	g_free(callback);
}

int session_pull(struct session_data *session,
				const char *type, const char *filename,
				session_callback_t function, void *user_data)
{
	struct callback_data *callback;
	GwObexXfer *xfer;

	if (session->obex == NULL)
		return -ENOTCONN;

	session_ref(session);

	callback = g_try_malloc0(sizeof(*callback));
	if (callback == NULL) {
		session_unref(session);
		return -ENOMEM;
	}

	callback->session = session;
	callback->func = function;
	callback->data = user_data;

	xfer = gw_obex_get_async(session->obex, NULL, type, NULL);
	if (xfer == NULL)
		return -ENOTCONN;

	gw_obex_xfer_set_callback(xfer, get_xfer_progress, callback);

	session->xfer = xfer;

	return 0;
}

int session_register(struct session_data *session)
{
	GDBusMethodTable *methods;
	const char *iface;

	switch (session->uuid) {
	case OBEX_FILETRANS_SVCLASS_ID:
		iface = FTP_INTERFACE;
		methods = ftp_methods;
		break;
	default:
		return -EINVAL;
	}

	session->path = g_strdup_printf("%s/session%ju",
					SESSION_BASEPATH, counter++);

	if (g_dbus_register_interface(session->conn, session->path,
				SESSION_INTERFACE,
				session_methods, NULL, NULL,
				session, NULL) == FALSE)
		return -EIO;

	if (g_dbus_register_interface(session->conn, session->path,
				iface,
				methods, NULL, NULL,
				session, NULL) == FALSE) {
		g_dbus_unregister_interface(session->conn,
				session->path, SESSION_INTERFACE);
		return -EIO;
	}

	session_ref(session);

	return 0;
}
