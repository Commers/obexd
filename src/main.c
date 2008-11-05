/*
 *
 *  OBEX Server
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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <getopt.h>
#include <syslog.h>
#include <glib.h>

#include <gdbus.h>

#include <openobex/obex.h>
#include <openobex/obex_const.h>

#include "logging.h"
#include "bluetooth.h"
#include "obexd.h"
#include "obex.h"

#define OPP_CHANNEL	9
#define FTP_CHANNEL	10
#define PBAP_CHANNEL	15

#define DEFAULT_ROOT_PATH "/tmp"

#define DEFAULT_CAP_FILE CONFIGDIR "/capability.xml"

static GMainLoop *main_loop = NULL;

int tty_init(int services, const gchar *root_path,
		const gchar *capability, const gchar *devnode)
{
	struct server *server;
	struct termios options;
	int fd, ret;
	glong flags;

	fd = open(devnode, O_RDWR);
	if (fd < 0)
		return fd;

	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	tcgetattr(fd, &options);
	cfmakeraw(&options);
	tcsetattr(fd, TCSANOW, &options);

	server = g_malloc0(sizeof(struct server));
	server->services = services;
	server->folder = g_strdup(root_path);
	server->auto_accept = TRUE;
	server->capability = g_strdup(capability);
	server->devnode = g_strdup(devnode);

	ret = obex_session_start(fd, server);
	if (ret < 0) {
		server_free(server);
		close(fd);
	}

	return ret;
}

static void sig_term(int sig)
{
	g_main_loop_quit(main_loop);
}

static gboolean option_detach = TRUE;
static gboolean option_debug = FALSE;

static gchar *option_root = NULL;
static gchar *option_capability = NULL;
static gchar *option_devnode = NULL;

static gboolean option_autoaccept = FALSE;
static gboolean option_opp = FALSE;
static gboolean option_ftp = FALSE;
static gboolean option_pbap = FALSE;

static GOptionEntry options[] = {
	{ "nodaemon", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Don't run as daemon in background" },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &option_debug,
				"Enable debug information output" },
	{ "root", 'r', 0, G_OPTION_ARG_STRING, &option_root,
				"Specify root folder location", "PATH" },
	{ "capability", 'c', 0, G_OPTION_ARG_STRING, &option_capability,
				"Sepcify capability file", "FILE" },
	{ "tty", 't', 0, G_OPTION_ARG_STRING, &option_devnode,
				"Specify the TTY device", "DEVICE" },
	{ "auto-accept", 'a', 0, G_OPTION_ARG_NONE, &option_autoaccept,
				"Automatically accept push requests" },
	{ "opp", 'o', 0, G_OPTION_ARG_NONE, &option_opp,
				"Enable Object Push server" },
	{ "ftp", 'f', 0, G_OPTION_ARG_NONE, &option_ftp,
				"Enable File Transfer server" },
	{ "pbap", 'p', 0, G_OPTION_ARG_NONE, &option_pbap,
				"Enable Phonebook Access server" },
	{ NULL },
};

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *err = NULL;
	struct sigaction sa;
	int log_option = LOG_NDELAY | LOG_PID, services = 0;

#ifdef NEED_THREADS
	if (g_thread_supported() == FALSE)
		g_thread_init(NULL);
#endif

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
		} else
			g_printerr("An unknown error occurred\n");
		exit(EXIT_FAILURE);
	}

	g_option_context_free(context);

	if (option_detach == TRUE) {
		if (daemon(0, 0)) {
			perror("Can't start daemon");
			exit(1);
		}
	} else
		log_option |= LOG_PERROR;

	if (option_opp == FALSE && option_ftp == FALSE &&
						option_pbap == FALSE) {
		fprintf(stderr, "No server selected (use either "
					"--opp or --ftp or both)\n");
		exit(EXIT_FAILURE);
	}

	openlog("obexd", log_option, LOG_DAEMON);

	if (option_debug == TRUE) {
		info("Enabling debug information");
		enable_debug();
	}

	main_loop = g_main_loop_new(NULL, FALSE);

#ifdef NEED_THREADS
	if (dbus_threads_init_default() == FALSE) {
		fprintf(stderr, "Can't init usage of threads\n");
		exit(EXIT_FAILURE);
	}
#endif

	if (manager_init() == FALSE) {
		error("manager_init failed");
		exit(EXIT_FAILURE);
	}

	plugin_init();

	if (option_root == NULL)
		option_root = g_strdup(DEFAULT_ROOT_PATH);

	if (option_capability == NULL)
		option_capability = g_strdup(DEFAULT_CAP_FILE);

	if (option_opp == TRUE) {
		services |= OBEX_OPP;
		bluetooth_init(OBEX_OPP, "Object Push server", option_root,
				OPP_CHANNEL, FALSE, option_autoaccept, NULL);
	}

	if (option_ftp == TRUE) {
		services |= OBEX_FTP;
		bluetooth_init(OBEX_FTP, "File Transfer server", option_root,
				FTP_CHANNEL, TRUE, option_autoaccept,
				option_capability);
	}

	if (option_pbap == TRUE) {
		services |= OBEX_PBAP;
		bluetooth_init(OBEX_PBAP, "Phonebook Access server", NULL,
				PBAP_CHANNEL, TRUE, FALSE, NULL);
	}

	if (option_devnode)
		tty_init(services, option_root, option_capability,
				option_devnode);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_term;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	g_main_loop_run(main_loop);

	bluetooth_exit();

	plugin_cleanup();

	manager_cleanup();

	g_main_loop_unref(main_loop);

	g_free(option_devnode);
	g_free(option_capability);
	g_free(option_root);

	closelog();

	return 0;
}
