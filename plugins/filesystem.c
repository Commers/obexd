/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2009-2010  Intel Corporation
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <wait.h>

#include <glib.h>

#include <openobex/obex.h>
#include <openobex/obex_const.h>

#include "plugin.h"
#include "logging.h"
#include "obex.h"
#include "mimetype.h"
#include "service.h"
#include "filesystem.h"

#define EOL_CHARS "\n"

#define FL_VERSION "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" EOL_CHARS

#define FL_TYPE "<!DOCTYPE folder-listing SYSTEM \"obex-folder-listing.dtd\">" EOL_CHARS

#define FL_TYPE_PCSUITE "<!DOCTYPE folder-listing SYSTEM \"obex-folder-listing.dtd\"" EOL_CHARS \
                        "  [ <!ATTLIST folder mem-type CDATA #IMPLIED> ]>" EOL_CHARS

#define FL_BODY_BEGIN "<folder-listing version=\"1.0\">" EOL_CHARS

#define FL_BODY_END "</folder-listing>" EOL_CHARS

#define FL_PARENT_FOLDER_ELEMENT "<parent-folder/>" EOL_CHARS

#define FL_FILE_ELEMENT "<file name=\"%s\" size=\"%lu\"" \
			" %s accessed=\"%s\" " \
			"modified=\"%s\" created=\"%s\"/>" EOL_CHARS

#define FL_FOLDER_ELEMENT "<folder name=\"%s\" %s accessed=\"%s\" " \
			"modified=\"%s\" created=\"%s\"/>" EOL_CHARS

#define FL_FOLDER_ELEMENT_PCSUITE "<folder name=\"%s\" %s accessed=\"%s\"" \
			" modified=\"%s\" mem-type=\"DEV\"" \
			" created=\"%s\"/>" EOL_CHARS

static const guint8 FTP_TARGET[TARGET_SIZE] = {
			0xF9, 0xEC, 0x7B, 0xC4,  0x95, 0x3C, 0x11, 0xD2,
			0x98, 0x4E, 0x52, 0x54,  0x00, 0xDC, 0x9E, 0x09  };

static gchar *file_stat_line(gchar *filename, struct stat *fstat,
				struct stat *dstat, gboolean root,
				gboolean pcsuite)
{
	gchar perm[51], atime[18], ctime[18], mtime[18];
	gchar *escaped, *ret = NULL;

	snprintf(perm, 50, "user-perm=\"%s%s%s\" group-perm=\"%s%s%s\" "
			"other-perm=\"%s%s%s\"",
			(fstat->st_mode & S_IRUSR ? "R" : ""),
			(fstat->st_mode & S_IWUSR ? "W" : ""),
			(dstat->st_mode & S_IWUSR ? "D" : ""),
			(fstat->st_mode & S_IRGRP ? "R" : ""),
			(fstat->st_mode & S_IWGRP ? "W" : ""),
			(dstat->st_mode & S_IWGRP ? "D" : ""),
			(fstat->st_mode & S_IROTH ? "R" : ""),
			(fstat->st_mode & S_IWOTH ? "W" : ""),
			(dstat->st_mode & S_IWOTH ? "D" : ""));

	strftime(atime, 17, "%Y%m%dT%H%M%SZ", gmtime(&fstat->st_atime));
	strftime(ctime, 17, "%Y%m%dT%H%M%SZ", gmtime(&fstat->st_ctime));
	strftime(mtime, 17, "%Y%m%dT%H%M%SZ", gmtime(&fstat->st_mtime));

	escaped = g_markup_escape_text(filename, -1);

	if (S_ISDIR(fstat->st_mode)) {
		if (pcsuite && root && g_str_equal(filename, "Data"))
			ret = g_strdup_printf(FL_FOLDER_ELEMENT_PCSUITE,
						escaped, perm, atime,
						mtime, ctime);
		else
			ret = g_strdup_printf(FL_FOLDER_ELEMENT, escaped, perm,
						atime, mtime, ctime);
	} else if (S_ISREG(fstat->st_mode))
		ret = g_strdup_printf(FL_FILE_ELEMENT, escaped, fstat->st_size,
					perm, atime, mtime, ctime);

	g_free(escaped);

	return ret;
}

static gpointer filesystem_open(const char *name, int oflag, mode_t mode,
		gpointer context, size_t *size, int *err)
{
	struct obex_session *os = context;
	struct stat stats;
	struct statvfs buf;
	const char *root_folder, *folder;
	gboolean root;
	int fd = open(name, oflag, mode);

	if (fd < 0) {
		if (err)
			*err = -errno;
		return NULL;
	}

	if (fstat(fd, &stats) < 0) {
		if (err)
			*err = -errno;
		goto failed;
	}

	root_folder = obex_get_root_folder(os);
	folder = g_path_get_dirname(name);
	root = g_strcmp0(folder, root_folder);

	if (!root || obex_get_symlinks(os)) {
		if (S_ISLNK(stats.st_mode)) {
			if (err)
				*err = -EPERM;
			goto failed;
		}

	}

	if (oflag == O_RDONLY) {
		if (size)
			*size = stats.st_size;
		goto done;
	}

	if (fstatvfs(fd, &buf) < 0) {
		if (err)
			*err = -errno;
		goto failed;
	}

	if (size == NULL)
		goto done;

	if (buf.f_bsize * buf.f_bavail < *size) {
		if (err)
			*err = -ENOSPC;
		goto failed;
	}

done:
	if (err)
		*err = 0;

	return GINT_TO_POINTER(fd);

failed:
	close(fd);
	return NULL;
}

static int filesystem_close(gpointer object)
{
	if (close(GPOINTER_TO_INT(object)) < 0)
		return -errno;

	return 0;
}

static ssize_t filesystem_read(gpointer object, void *buf, size_t count)
{
	ssize_t ret;

	ret = read(GPOINTER_TO_INT(object), buf, count);
	if (ret < 0)
		return -errno;

	return ret;
}

static ssize_t filesystem_write(gpointer object, const void *buf, size_t count)
{
	ssize_t ret;

	ret = write(GPOINTER_TO_INT(object), buf, count);
	if (ret < 0)
		return -errno;

	return ret;
}

struct capability_object {
	int pid;
	int output;
	int err;
	guint watch;
	GString *buffer;
};

static void script_exited(GPid pid, gint status, gpointer data)
{
	struct capability_object *object = data;
	char buf[128];

	object->pid = -1;

	if (WEXITSTATUS(status) != EXIT_SUCCESS) {
		memset(buf, 0, sizeof(buf));
		if (read(object->err, buf, sizeof(buf)) > 0)
			error("%s", buf);
		obex_object_set_io_flags(data, G_IO_ERR, -EPERM);
	} else
		obex_object_set_io_flags(data, G_IO_IN, 0);

	g_spawn_close_pid(pid);
}

static int capability_exec(const char **argv, int *output, int *err)
{
	GError *gerr = NULL;
	int pid;
	GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;

	if (!g_spawn_async_with_pipes(NULL, (char **) argv, NULL, flags, NULL,
				NULL, &pid, NULL, output, err, &gerr)) {
		error("%s", gerr->message);
		g_error_free(gerr);
		return -EPERM;
	}

	return pid;
}

static gpointer capability_open(const char *name, int oflag, mode_t mode,
		gpointer context, size_t *size, int *err)
{
	struct capability_object *object = NULL;
	gchar *buf;
	const char *argv[2];

	if (oflag != O_RDONLY)
		goto fail;

	object = g_new0(struct capability_object, 1);
	object->pid = -1;
	object->output = -1;
	object->err = -1;

	if (name[0] != '!') {
		GError *gerr = NULL;
		gboolean ret;

		ret = g_file_get_contents(name, &buf, NULL, &gerr);
		if (ret == FALSE) {
			error("%s", gerr->message);
			g_error_free(gerr);
			goto fail;
		}

		object->buffer = g_string_new(buf);

		if (size)
			*size = object->buffer->len;

		goto done;
	}

	argv[0] = &name[1];
	argv[1] = NULL;

	object->pid = capability_exec(argv, &object->output, &object->err);
	if (object->pid < 0)
		goto fail;

	object->watch = g_child_watch_add(object->pid, script_exited, object);

	if (size)
		*size = 1;

done:
	if (err)
		*err = 0;

	return object;

fail:
	if (err)
		*err = -EPERM;

	g_free(object);
	return NULL;
}

static gpointer folder_open(const char *name, int oflag, mode_t mode,
		 gpointer context, size_t *size, int *err)
{
	struct obex_session *os = context;
	struct stat fstat, dstat;
	struct dirent *ep;
	GString *object;
	DIR *dp;
	gboolean root, pcsuite, symlinks;
	gint ret;

	pcsuite = obex_get_service(os) & OBEX_PCSUITE ? TRUE : FALSE;

	object = g_string_new(FL_VERSION);
	object = g_string_append(object, pcsuite ? FL_TYPE_PCSUITE : FL_TYPE);

	object = g_string_append(object, FL_BODY_BEGIN);

	root = g_str_equal(name, obex_get_root_folder(os));

	dp = opendir(name);
	if (dp == NULL) {
		if (err)
			*err = -ENOENT;
		goto failed;
	}

	symlinks = obex_get_symlinks(os);
	if (root && symlinks)
		ret = stat(name, &dstat);
	else {
		object = g_string_append(object, FL_PARENT_FOLDER_ELEMENT);
		ret = lstat(name, &dstat);
	}

	if (ret < 0) {
		if (err)
			*err = -errno;
		goto failed;
	}

	while ((ep = readdir(dp))) {
		gchar *filename;
		gchar *fullname;
		gchar *line;

		if (ep->d_name[0] == '.')
			continue;

		filename = g_filename_to_utf8(ep->d_name, -1, NULL, NULL, NULL);
		if (name == NULL) {
			error("g_filename_to_utf8: invalid filename");
			continue;
		}

		fullname = g_build_filename(name, ep->d_name, NULL);

		if (root && symlinks)
			ret = stat(fullname, &fstat);
		else
			ret = lstat(fullname, &fstat);

		if (ret < 0) {
			debug("%s: %s(%d)", root ? "stat" : "lstat",
					strerror(errno), errno);
			g_free(filename);
			g_free(fullname);
			continue;
		}

		g_free(fullname);

		line = file_stat_line(filename, &fstat, &dstat, root, pcsuite);
		if (line == NULL) {
			g_free(filename);
			continue;
		}

		g_free(filename);

		object = g_string_append(object, line);
		g_free(line);
	}

	closedir(dp);

	object = g_string_append(object, FL_BODY_END);
	if (size)
		*size = object->len;

	if (err)
		*err = 0;

	return object;

failed:
	if (dp)
		closedir(dp);

	g_string_free(object, TRUE);
	return NULL;
}

int string_free(gpointer object)
{
	GString *string = object;

	g_string_free(string, TRUE);

	return 0;
}

ssize_t string_read(gpointer object, void *buf, size_t count)
{
	GString *string = object;
	ssize_t len;

	if (string->len == 0)
		return 0;

	len = MIN(string->len, count);
	memcpy(buf, string->str, len);
	string = g_string_erase(string, 0, len);

	return len;
}

static ssize_t capability_read(gpointer object, void *buf, size_t count)
{
	struct capability_object *obj = object;

	if (obj->buffer)
		return string_read(obj->buffer, buf, count);

	if (obj->pid >= 0)
		return -EAGAIN;

	return read(obj->output, buf, count);
}

static int capability_close(gpointer object)
{
	struct capability_object *obj = object;

	if (obj->pid >= 0) {
		g_source_remove(obj->watch);
		kill(obj->pid, SIGTERM);
		g_spawn_close_pid(obj->pid);
	}

	if (obj->buffer != NULL)
		g_string_free(obj->buffer, TRUE);

	g_free(obj);

	return 0;
}

static struct obex_mime_type_driver file = {
	.open = filesystem_open,
	.close = filesystem_close,
	.read = filesystem_read,
	.write = filesystem_write,
	.remove = remove,
};

static struct obex_mime_type_driver capability = {
	.target = FTP_TARGET,
	.mimetype = "x-obex/capability",
	.open = capability_open,
	.close = capability_close,
	.read = capability_read,
};

static struct obex_mime_type_driver folder = {
	.target = FTP_TARGET,
	.mimetype = "x-obex/folder-listing",
	.open = folder_open,
	.close = string_free,
	.read = string_read,
};

static int filesystem_init(void)
{
	int err;

	err = obex_mime_type_driver_register(&folder);
	if (err < 0)
		return err;

	err = obex_mime_type_driver_register(&capability);
	if (err < 0)
		return err;

	return obex_mime_type_driver_register(&file);
}

static void filesystem_exit(void)
{
	obex_mime_type_driver_unregister(&folder);
	obex_mime_type_driver_unregister(&capability);
	obex_mime_type_driver_unregister(&file);
}

OBEX_PLUGIN_DEFINE(filesystem, filesystem_init, filesystem_exit)
