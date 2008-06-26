/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2007-2008  Nokia Corporation
 *  Copyright (C) 2007-2008  Instituto Nokia de Tecnologia (INdT)
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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>

#include <glib.h>

#include <openobex/obex.h>
#include <openobex/obex_const.h>

#include "logging.h"
#include "obex.h"
#include "dbus.h"

/* Default MTU's */
#define RX_MTU 32767
#define TX_MTU 32767

#define TARGET_SIZE	16
static const guint8 FTP_TARGET[TARGET_SIZE] = { 0xF9, 0xEC, 0x7B, 0xC4,
					0x95, 0x3C, 0x11, 0xD2,
					0x98, 0x4E, 0x52, 0x54,
					0x00, 0xDC, 0x9E, 0x09 };

/* Connection ID */
static guint32 cid = 0x0000;

typedef struct {
    guint8	version;
    guint8	flags;
    guint16	mtu;
} __attribute__ ((packed)) obex_connect_hdr_t;

struct obex_commands opp = {
	.get		= opp_get,
	.chkput		= opp_chkput,
	.put		= opp_put,
	.setpath	= NULL,
};

struct obex_commands ftp = {
	.get		= ftp_get,
	.put		= ftp_put,
	.setpath	= ftp_setpath,
};

static void obex_session_free(struct obex_session *os)
{
	if (os->name)
		g_free(os->name);
	if (os->type)
		g_free(os->type);
	if (os->current_folder)
		g_free(os->current_folder);
	if (os->buf)
		g_free(os->buf);
	if (os->fd > 0)
		close(os->fd);
	g_free(os);
}

static void cmd_connect(struct obex_session *os,
			obex_t *obex, obex_object_t *obj)
{
	obex_connect_hdr_t *nonhdr;
	obex_headerdata_t hd;
	uint8_t *buffer;
	guint hlen, newsize;
	guint16 mtu;
	guint8 hi;

	if (OBEX_ObjectGetNonHdrData(obj, &buffer) != sizeof(*nonhdr)) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_FORBIDDEN, OBEX_RSP_FORBIDDEN);
		debug("Invalid OBEX CONNECT packet");
		return;
	}

	nonhdr = (obex_connect_hdr_t *) buffer;
	mtu = g_ntohs(nonhdr->mtu);
	debug("Version: 0x%02x. Flags: 0x%02x  OBEX packet length: %d",
			nonhdr->version, nonhdr->flags, mtu);
	/* Leave space for headers */
	newsize = mtu - 200;

	os->tx_mtu = newsize;

	debug("Resizing stream chunks to %d", newsize);

	/* connection id will be used to track the sessions, even for OPP */
	os->cid = ++cid;

	if (os->target == NULL) {
		/* OPP doesn't contains target or connection id. */
		OBEX_ObjectSetRsp(obj, OBEX_RSP_CONTINUE, OBEX_RSP_SUCCESS);
		return;
	}

	hi = hlen = 0;
	OBEX_ObjectGetNextHeader(obex, obj, &hi, &hd, &hlen);

	if (hi != OBEX_HDR_TARGET || hlen != TARGET_SIZE
			|| memcmp(os->target, hd.bs, TARGET_SIZE) != 0) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_FORBIDDEN, OBEX_RSP_FORBIDDEN);
		return;
	}

	/* Append received UUID in WHO header */
	OBEX_ObjectAddHeader(obex, obj,
			OBEX_HDR_WHO, hd, TARGET_SIZE,
			OBEX_FL_FIT_ONE_PACKET);
	hd.bq4 = cid;
	OBEX_ObjectAddHeader(obex, obj,
			OBEX_HDR_CONNECTION, hd, 4,
			OBEX_FL_FIT_ONE_PACKET);

	OBEX_ObjectSetRsp(obj, OBEX_RSP_CONTINUE, OBEX_RSP_SUCCESS);
}

static gboolean chk_cid(obex_t *obex, obex_object_t *obj, guint32 cid)
{
	struct obex_session *os;
	obex_headerdata_t hd;
	guint hlen;
	guint8 hi;
	gboolean ret = FALSE;

	os = OBEX_GetUserData(obex);

	/* OPUSH doesn't provide a connection id. */
	if (os->target == NULL)
		return TRUE;

	while (OBEX_ObjectGetNextHeader(obex, obj, &hi, &hd, &hlen)) {
		if (hi == OBEX_HDR_CONNECTION && hlen == 4) {
			ret = (hd.bq4 == cid ? TRUE : FALSE);
			break;
		}
	}

	if (ret == FALSE)
		OBEX_ObjectSetRsp(obj, OBEX_RSP_SERVICE_UNAVAILABLE,
				OBEX_RSP_SERVICE_UNAVAILABLE);
	else
		OBEX_ObjectReParseHeaders(obex, obj);

	return ret;
}

static void cmd_get(struct obex_session *os, obex_t *obex, obex_object_t *obj)
{
	obex_headerdata_t hd;
	guint hlen;
	guint8 hi;

	if (!os->cmds->get) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_NOT_IMPLEMENTED,
				OBEX_RSP_NOT_IMPLEMENTED);
		return;
	}

	g_return_if_fail(chk_cid(obex, obj, os->cid));

	if (os->type) {
		g_free(os->type);
		os->type = NULL;
	}

	if (os->name) {
		g_free(os->name);
		os->name = NULL;
	}

	if (os->buf) {
		g_free(os->buf);
		os->buf = NULL;
	}

	while (OBEX_ObjectGetNextHeader(obex, obj, &hi, &hd, &hlen)) {
		switch (hi) {
		case OBEX_HDR_NAME:
			if (hlen == 0)
				continue;

			os->name = g_convert((const gchar *) hd.bs, hlen,
					"UTF8", "UTF16BE", NULL, NULL, NULL);
			debug("OBEX_HDR_NAME: %s", os->name);
			break;
		case OBEX_HDR_TYPE:
			if (hlen == 0)
				continue;

			/* Ensure null termination */
			if (hd.bs[hlen - 1] != '\0')
				break;

			if (!g_utf8_validate((const gchar *) hd.bs, -1, NULL)) {
				debug("Invalid type header: %s", hd.bs);
				break;
			}

			os->type = g_strndup((const gchar *) hd.bs, hlen);
			debug("OBEX_HDR_TYPE: %s", os->type);
			break;
		}
	}

	if (!os->name) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_BAD_REQUEST,
				OBEX_RSP_BAD_REQUEST);
		g_free(os->type);
		os->type = NULL;
		return;
	}

	if (!os->type) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_BAD_REQUEST,
				OBEX_RSP_BAD_REQUEST);
		g_free(os->name);
		os->name = NULL;
		return;
	}

	os->cmds->get(obex, obj);
}

static void cmd_put(struct obex_session *os, obex_t *obex, obex_object_t *obj)
{
	if (!os->cmds->put) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_NOT_IMPLEMENTED,
				OBEX_RSP_NOT_IMPLEMENTED);
		return;
	}

	g_return_if_fail(chk_cid(obex, obj, os->cid));

	os->cmds->put(obex, obj);
}

static void cmd_setpath(struct obex_session *os,
			obex_t *obex, obex_object_t *obj)
{
	obex_headerdata_t hd;
	guint32 hlen;
	guint8 hi;

	if (!os->cmds->setpath) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_NOT_IMPLEMENTED,
				OBEX_RSP_NOT_IMPLEMENTED);
		return;
	}

	g_return_if_fail(chk_cid(obex, obj, os->cid));

	if (os->name) {
		g_free(os->name);
		os->name = NULL;
	}

	while (OBEX_ObjectGetNextHeader(obex, obj, &hi, &hd, &hlen)) {
		if (hi == OBEX_HDR_NAME) {
			/*
			 * This is because OBEX_UnicodeToChar() accesses
			 * the string even if its size is zero
			 */
			if (hlen == 0) {
				os->name = g_strdup("");
				break;
			}

			os->name = g_convert((const gchar *) hd.bs, hlen,
					"UTF8", "UTF16BE", NULL, NULL, NULL);

			debug("Set path name: %s", os->name);
			break;
		}
	}

	if (!os->name) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_BAD_REQUEST,
				OBEX_RSP_BAD_REQUEST);
		return;
	}

	os->cmds->setpath(obex, obj);
}

gint os_setup_by_name(struct obex_session *os, gchar *file)
{
	gint fd;
	struct stat stats;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		goto fail;

	if (fstat(fd, &stats))
		goto fail;

	os->fd = fd;
	os->buf = g_new0(guint8, os->tx_mtu);
	os->offset = 0;

	return stats.st_size;

fail:
	if (fd >= 0)
		close(fd);

	return 0;
}

static gint obex_write(struct obex_session *os,
			obex_t *obex, obex_object_t *obj)
{
	obex_headerdata_t hd;
	gint32 len;

	debug("obex_write name: %s type: %s tx_mtu: %d fd: %d",
			os->name, os->type, os->tx_mtu, os->fd);

	if (os->fd < 0)
		return -EIO;

	len = read(os->fd, os->buf, os->tx_mtu);
	if (len < 0) {
		gint err = errno;
		g_free(os->buf);
		return -err;
	}

	os->offset += len;

	if (len == 0) {
		OBEX_ObjectAddHeader(obex, obj, OBEX_HDR_BODY, hd, 0,
					OBEX_FL_STREAM_DATAEND);
		g_free(os->buf);
		os->buf = NULL;
		return len;
	}

	hd.bs = os->buf;
	OBEX_ObjectAddHeader(obex, obj, OBEX_HDR_BODY, hd, len,
				OBEX_FL_STREAM_DATA);

	return len;
}

static gint obex_read(struct obex_session *os,
			obex_t *obex, obex_object_t *obj)
{
	gint size;
	gint32 len = 0;
	const guint8 *buffer;

	if (os->fd < 0)
		return -EIO;

	size = OBEX_ObjectReadStream(obex, obj, &buffer);
	if (size <= 0) {
		close(os->fd);
		return 0;
	}

	if (size > os->rx_mtu) {
		error("Received more data than RX_MAX");
		return -EIO;
	}

	if (os->fd < 0) {
		if (os->buf) {
			error("Got more data but there is still a pending buffer");
			return -EIO;
		}

		os->buf = g_malloc0(os->rx_mtu);
		memcpy(os->buf, buffer, size);
		os->offset += size;
		return 0;
	}

	while (len < size) {
		gint w;

		w = write(os->fd, buffer + len, size - len);
		if (w < 0) {
			gint err = errno;
			if (err == EINTR)
				continue;
			else
				return -err;
		}

		len += w;
	}

	os->offset += len;

	return 0;
}

static void check_put(obex_t *obex, obex_object_t *obj)
{
	struct obex_session *os;
	struct statvfs buf;
	obex_headerdata_t hd;
	guint hlen;
	guint8 hi;
	guint64 free;

	os = OBEX_GetUserData(obex);

	if (os->type) {
		g_free(os->type);
		os->type = NULL;
	}

	if (os->name) {
		g_free(os->name);
		os->name = NULL;
	}

	while (OBEX_ObjectGetNextHeader(obex, obj, &hi, &hd, &hlen)) {
		switch (hi) {
		case OBEX_HDR_NAME:
			if (hlen == 0)
				continue;

			os->name = g_convert((const gchar *) hd.bs, hlen,
					"UTF8", "UTF16BE", NULL, NULL, NULL);
			debug("OBEX_HDR_NAME: %s", os->name);
			break;

		case OBEX_HDR_TYPE:
			if (hlen == 0)
				continue;

			/* Ensure null termination */
			if (hd.bs[hlen - 1] != '\0')
				break;

			if (!g_utf8_validate((const gchar *) hd.bs, -1, NULL)) {
				debug("Invalid type header: %s", hd.bs);
				break;
			}

			os->type = g_strndup((const gchar *) hd.bs, hlen);
			debug("OBEX_HDR_TYPE: %s", os->type);
			break;

		case OBEX_HDR_BODY:
			os->size = -1;
			break;

		case OBEX_HDR_LENGTH:
			os->size = hd.bq4;
			debug("OBEX_HDR_LENGTH: %d", os->size);
			break;
		}
	}

	if (!os->name) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_BAD_REQUEST,
				OBEX_RSP_BAD_REQUEST);
		g_free(os->type);
		os->type = NULL;
		return;
	}

	if (!os->type) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_BAD_REQUEST,
				OBEX_RSP_BAD_REQUEST);
		g_free(os->name);
		os->name = NULL;
		return;
	}

	if (!os->cmds->chkput)
		return;

	if (os->cmds->chkput(obex, obj) < 0) {
		OBEX_ObjectSetRsp(obj, OBEX_RSP_FORBIDDEN, OBEX_RSP_FORBIDDEN);
		return;
	}

	if (fstatvfs(os->fd, &buf) < 0) {
		int err = errno;
		error("fstatvfs(): %s(%d)", strerror(err), err);
		OBEX_ObjectSetRsp(obj, OBEX_RSP_FORBIDDEN, OBEX_RSP_FORBIDDEN);
		return;
	}

	free = buf.f_bsize * buf.f_bavail;
	debug("Free space in disk: %lu", free);
	if (os->size > free) {
		debug("Free disk space not available");
		OBEX_ObjectSetRsp(obj, OBEX_RSP_FORBIDDEN, OBEX_RSP_FORBIDDEN);
		return;
	}

	OBEX_ObjectReParseHeaders(obex, obj);
}

static void obex_event(obex_t *obex, obex_object_t *obj, gint mode,
					gint evt, gint cmd, gint rsp)
{
	struct obex_session *os;

	obex_debug(evt, cmd, rsp);

	switch (evt) {
	case OBEX_EV_PROGRESS:
		os = OBEX_GetUserData(obex);
		emit_transfer_progress(os->cid, os->size, os->offset);
		break;
	case OBEX_EV_ABORT:
		OBEX_ObjectSetRsp(obj, OBEX_RSP_SUCCESS, OBEX_RSP_SUCCESS);
		break;
	case OBEX_EV_REQDONE:
		switch (cmd) {
		case OBEX_CMD_DISCONNECT:
			OBEX_TransportDisconnect(obex);
			break;
		case OBEX_CMD_PUT:
		case OBEX_CMD_GET:
			break;
		default:
			break;
		}
		break;
	case OBEX_EV_REQHINT:
		switch (cmd) {
		case OBEX_CMD_PUT:
			OBEX_ObjectReadStream(obex, obj, NULL);
		case OBEX_CMD_GET:
		case OBEX_CMD_SETPATH:
		case OBEX_CMD_CONNECT:
		case OBEX_CMD_DISCONNECT:
			OBEX_ObjectSetRsp(obj, OBEX_RSP_CONTINUE,
					OBEX_RSP_SUCCESS);
			break;
		default:
			OBEX_ObjectSetRsp(obj, OBEX_RSP_NOT_IMPLEMENTED,
					OBEX_RSP_NOT_IMPLEMENTED);
			break;
		}
		break;
	case OBEX_EV_REQCHECK:
		switch (cmd) {
		case OBEX_CMD_PUT:
			os = OBEX_GetUserData(obex);
			if (os->cmds->put)
				check_put(obex, obj);
			break;
		default:
			break;
		}
		break;
	case OBEX_EV_REQ:
		os = OBEX_GetUserData(obex);
		switch (cmd) {
		case OBEX_CMD_DISCONNECT:
			break;
		case OBEX_CMD_CONNECT:
			cmd_connect(os, obex, obj);
			break;
		case OBEX_CMD_SETPATH:
			cmd_setpath(os, obex, obj);
			break;
		case OBEX_CMD_GET:
			cmd_get(os, obex, obj);
			break;
		case OBEX_CMD_PUT:
			cmd_put(os, obex, obj);
			break;
		default:
			debug("Unknown request: 0x%X", cmd);
			OBEX_ObjectSetRsp(obj,
				OBEX_RSP_NOT_IMPLEMENTED, OBEX_RSP_NOT_IMPLEMENTED);
			break;
		}
		break;
	case OBEX_EV_STREAMAVAIL:
		os = OBEX_GetUserData(obex);
		if (obex_read(os, obex, obj) < 0) {
			debug("error obex_read()");
			OBEX_CancelRequest(obex, 1);
		}
		break;
	case OBEX_EV_STREAMEMPTY:
		os = OBEX_GetUserData(obex);
		obex_write(os, obex, obj);
		break;
	case OBEX_EV_LINKERR:
		break;
	case OBEX_EV_PARSEERR:
		break;
	case OBEX_EV_UNEXPECTED:
		break;

	default:
		debug("Unknown evt %d", evt);
		break;
	}
}

static void obex_handle_destroy(gpointer user_data)
{
	struct obex_session *os;
	obex_t *obex = user_data;

	os = OBEX_GetUserData(obex);

	emit_transfer_completed(os->cid, os->offset == os->size);

	obex_session_free(os);

	OBEX_Cleanup(obex);
}

static gboolean obex_handle_input(GIOChannel *io,
				GIOCondition cond, gpointer user_data)
{
	obex_t *obex = user_data;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	if (OBEX_HandleInput(obex, 1) < 0) {
		error("Handle input error");
		return FALSE;
	}

	return TRUE;
}

gint obex_server_start(gint fd, gint mtu, struct server *server)
{
	struct obex_session *os;
	GIOChannel *io;
	obex_t *obex;
	gint ret;

	os = g_new0(struct obex_session, 1);
	switch (server->service) {
	case OBEX_OPUSH:
		os->target = NULL;
		os->cmds = &opp;
		break;
	case OBEX_FTP:
		os->target = FTP_TARGET;
		os->cmds = &ftp;
		break;
	default:
		g_free(os);
		debug("Invalid OBEX server");
		return -EINVAL;
	}

	os->current_folder = g_strdup(server->folder);
	os->server = server;
	os->rx_mtu = RX_MTU;
	os->tx_mtu = TX_MTU;

	obex = OBEX_Init(OBEX_TRANS_FD, obex_event, 0);
	if (!obex)
		return -EIO;

	OBEX_SetUserData(obex, os);

	OBEX_SetTransportMTU(obex, os->rx_mtu, os->tx_mtu);

	ret = FdOBEX_TransportSetup(obex, fd, fd, 0);
	if (ret < 0) {
		obex_session_free(os);
		OBEX_Cleanup(obex);
		return ret;
	}

	io = g_io_channel_unix_new(fd);
	g_io_add_watch_full(io, G_PRIORITY_DEFAULT,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			obex_handle_input, obex, obex_handle_destroy);
	g_io_channel_unref(io);

	return 0;
}

gint obex_server_stop()
{
	return 0;
}
