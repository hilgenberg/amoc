/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <popt.h>

#include "server/server.h"
#include "server/input/decoder.h"
#include "client/client.h"
#include "server/protocol.h"
#include "Socket.h"

struct parameters
{
	int debug;
	int only_server;
	int foreground;
	int play;
	int allow_iface;
	int stop;
	int exit;
	int pause;
	int unpause;
	int next;
	int previous;
	int rate;
	int toggle_pause;
	int playit;
	int rating;
};

/* Connect to the server, return fd of the socket or -1 on error. */
static int server_connect ()
{
	/* Create a socket.
	 * For reasons why AF_UNIX is the correct constant to use in both
	 * cases, see the commentary the SVN log for commit r2833. */
	struct sockaddr_un sock_name;
	int sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1) return -1;

	sock_name.sun_family = AF_UNIX;
	strcpy (sock_name.sun_path, options::SocketPath.c_str());

	if (connect(sock, (sockaddr *)&sock_name, SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Ping the server.
 * Return 1 if the server responds with EV_PONG, otherwise 0. */
static bool ping_server (Socket &srv)
{
	srv.send(CMD_PING);
	return srv.get_int() == EV_PONG;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	const char *dir_name = options::RunDir.c_str();
	struct stat file_stat;

	if (stat (dir_name, &file_stat) == -1) {
		if (errno != ENOENT)
			fatal ("Error trying to check for RunDir directory: %s",
			        xstrerror (errno));

		if (mkdir (dir_name, 0700) == -1)
			fatal ("Can't create directory %s: %s",
					dir_name, xstrerror (errno));
	}
	else {
		if (!S_ISDIR(file_stat.st_mode) || access (dir_name, W_OK))
			fatal ("%s is not a writable directory!", dir_name);
	}
}

/* Run client and the server if needed. */
static void start_moc (const struct parameters *params, strings &args)
{
	if (params->foreground) {
		set_me_server ();
		options::load(SERVER);
		server_init (params->debug, params->foreground);
		server_loop ();
		options::save(SERVER);
		return;
	}

	int server_sock = server_connect ();

	if (server_sock != -1 && params->only_server)
		fatal ("Server is already running!");

	if (server_sock == -1) {
		int i = 0;
		int notify_pipe[2];
		ssize_t rc;

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", xstrerror (errno));

		switch (fork()) {
		case 0: /* child - start server */
			set_me_server ();
			options::load(SERVER);
			server_init (params->debug, params->foreground);
			rc = write (notify_pipe[1], &i, sizeof(i));
			if (rc < 0)
				fatal ("write() to notify pipe failed: %s", xstrerror (errno));
			close (notify_pipe[0]);
			close (notify_pipe[1]);
			server_loop ();
			options::save(SERVER);
			decoder_cleanup ();
			io_cleanup ();
			files_cleanup ();
			exit (EXIT_SUCCESS);
		case -1:
			fatal ("fork() failed: %s", xstrerror (errno));
		default:
			close (notify_pipe[1]);
			if (read(notify_pipe[0], &i, sizeof(i)) != sizeof(i))
				fatal ("Server exited!");
			close (notify_pipe[0]);
			server_sock = server_connect ();
			if (server_sock == -1) {
				perror ("server_connect()");
				fatal ("Can't connect to the server!");
			}
		}
	}

	Socket srv(server_sock);
	if (params->only_server)
		srv.send(CMD_DISCONNECT);
	else {
		xsignal (SIGPIPE, SIG_IGN);
		if (!ping_server (srv))
			fatal ("Can't connect to the server!");

		#define CLIENT_LOG "amoc_client_log"

		FILE *logfp = NULL;
		if (params->debug) {
			logfp = fopen (CLIENT_LOG, "a");
			if (!logfp) fatal ("Can't open client log file: %s", xstrerror (errno));
		}
		log_init_stream (logfp, CLIENT_LOG);

		options::load(GUI);
		{
			Client client(server_sock, args);
			client.run();
		}
		options::save(GUI);
		log_close();
	}

	close (server_sock);
}

void interface_cmdline_play_first (Socket &srv)
{
	srv.send(CMD_PLAY);
	srv.send(-1);
	srv.send("");
}

void interface_cmdline_playit (Socket &srv, strings &args)
{
	if (args.size() != 1) return;
	srv.send(CMD_PLAY);
	srv.send(-1);
	srv.send(args[0]);
}

void interface_cmdline_set_rating (Socket &srv, int rating)
{
	if (rating < 0) rating = 0;
	if (rating > 5) rating = 5;

	srv.send(CMD_SET_RATING);
	srv.send("");
	srv.send(rating);
}

/* Send commands requested in params to the server. */
static void server_command (struct parameters *params, strings &args)
{
	int srv_sock = server_connect();
	if (srv_sock == -1) fatal ("The server is not running!");

	xsignal (SIGPIPE, SIG_IGN);
	Socket srv(srv_sock);
	if (!ping_server (srv)) fatal ("Can't connect to the server!");

	if (params->playit) interface_cmdline_playit (srv, args);
	else if (params->play) interface_cmdline_play_first (srv);
	else if (params->rate) interface_cmdline_set_rating (srv, params->rating);
	else if (params->exit) srv.send(CMD_QUIT);
	else if (params->stop) srv.send(CMD_STOP);
	else if (params->pause) srv.send(CMD_PAUSE);
	else if (params->next) srv.send(CMD_NEXT);
	else if (params->previous) srv.send(CMD_PREV);
	else if (params->unpause) srv.send(CMD_UNPAUSE);
	else if (params->toggle_pause) {
		srv.send(CMD_GET_STATE);
		// this should be wait_for_data()...
		int ev = srv.get_int(), state = srv.get_int();
		if (ev != EV_DATA) fatal("Can't get state");

		if (state == STATE_PAUSE)
			srv.send(CMD_UNPAUSE);
		else if (state == STATE_PLAY)
			srv.send(CMD_PAUSE);
	}

	srv.send(CMD_DISCONNECT);
	close (srv_sock);
}

static void show_version ()
{
	int rc;
	struct utsname uts;

	putchar ('\n');
	printf ("          This is : %s\n", PACKAGE_NAME);
	printf ("          Version : %s\n", PACKAGE_VERSION);

	/* Show build time */
#ifdef __DATE__
	printf ("            Built : %s", __DATE__);
# ifdef __TIME__
	printf (" %s", __TIME__);
# endif
	putchar ('\n');
#endif

	rc = uname (&uts);
	if (rc == 0)
		printf ("       Running on : %s %s %s\n", uts.sysname, uts.release,
	                                                           uts.machine);

	printf ("           Author : Damian Pietras\n");
	printf ("         Homepage : %s\n", PACKAGE_URL);
	printf ("           E-Mail : %s\n", PACKAGE_BUGREPORT);
	printf ("        Copyright : (C) 2003-2016 Damian Pietras and others\n");
	printf ("          License : GNU General Public License, version 2 or later\n");
	putchar ('\n');
}

/* Show program banner. */
static void show_banner ()
{
	printf ("%s (version %s)\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static const char mocp_summary[] = "[OPTIONS] [FILE|DIR ...]";

/* Show program usage. */
static void show_usage (poptContext ctx)
{
	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintUsage (ctx, stdout, 0);
}

/* Show program help. */
static void show_help (poptContext ctx)
{
	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintHelp (ctx, stdout, 0);
}

/* Disambiguate the user's request. */
static void show_misc_cb (poptContext ctx,
                          enum poptCallbackReason unused1,
                          const struct poptOption *opt,
                          const char *unused2,
                          void *unused3)
{
	switch (opt->shortName) {
	case 'V':
		show_version ();
		break;
	case 'h':
		show_help (ctx);
		break;
	case 0:
		if (!strcmp (opt->longName, "usage"))
			show_usage (ctx);
		break;
	}

	exit (EXIT_SUCCESS);
}

enum {
	CL_HANDLED = 0,
	CL_NOIFACE,
	CL_RATE
};

static struct parameters params;

static struct poptOption general_opts[] = {
#ifndef NDEBUG
	{"debug", 'D', POPT_ARG_NONE, &params.debug, CL_HANDLED,
			"Turn on logging to a file", NULL},
#endif
	{"foreground", 'F', POPT_ARG_NONE, &params.foreground, CL_HANDLED,
			"Run the server in foreground (logging to stdout)", NULL},
	{"server", 'S', POPT_ARG_NONE, &params.only_server, CL_HANDLED,
			"Only run the server", NULL},
	POPT_TABLEEND
};

static struct poptOption server_opts[] = {
	{"pause", 'P', POPT_ARG_NONE, &params.pause, CL_NOIFACE,
			"Pause", NULL},
	{"unpause", 'U', POPT_ARG_NONE, &params.unpause, CL_NOIFACE,
			"Unpause", NULL},
	{"toggle-pause", 'G', POPT_ARG_NONE, &params.toggle_pause, CL_NOIFACE,
			"Toggle between playing and paused", NULL},
	{"stop", 's', POPT_ARG_NONE, &params.stop, CL_NOIFACE,
			"Stop playing", NULL},
	{"next", 'f', POPT_ARG_NONE, &params.next, CL_NOIFACE,
			"Play the next song", NULL},
	{"previous", 'r', POPT_ARG_NONE, &params.previous, CL_NOIFACE,
			"Play the previous song", NULL},
	{"rate", 'R', POPT_ARG_INT, &params.rating, CL_RATE,
			"Rate current song N stars (N=0..5)", "N"},
	{"exit", 'x', POPT_ARG_NONE, &params.exit, CL_NOIFACE,
			"Shutdown the server", NULL},
	{"play", 'p', POPT_ARG_NONE, &params.play, CL_NOIFACE,
			"Start playing from the first item on the playlist", NULL},
	{"playit", 'l', POPT_ARG_NONE, &params.playit, CL_NOIFACE,
			"Play files given on command line without modifying the playlist", NULL},
	POPT_TABLEEND
};

static struct poptOption misc_opts[] = {
	{NULL, 0, POPT_ARG_CALLBACK,
	       (void *) (uintptr_t) show_misc_cb, 0, NULL, NULL},
	{"version", 'V', POPT_ARG_NONE, NULL, 0,
			"Print version information", NULL},
	{"usage", 0, POPT_ARG_NONE, NULL, 0,
			"Print brief usage", NULL},
	{"help", 'h', POPT_ARG_NONE, NULL, 0,
			"Print extended usage", NULL},
	POPT_TABLEEND
};

static struct poptOption mocp_opts[] = {
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, general_opts, 0, "General options:", NULL},
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, server_opts, 0, "Server commands:", NULL},
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, misc_opts, 0, "Miscellaneous options:", NULL},
	POPT_AUTOALIAS
	POPT_TABLEEND
};

/* Process the command line options. */
static void process_options (poptContext ctx)
{
	int rc;

	while ((rc = poptGetNextOpt (ctx)) >= 0) {
		const char *arg = poptGetOptArg (ctx);

		switch (rc) {
		case CL_NOIFACE:
			params.allow_iface = 0;
			break;
		case CL_RATE:
			params.rate = 1;
			params.allow_iface = 0;
			break;
		default:
			show_usage (ctx);
			exit (EXIT_FAILURE);
		}

		free ((void *) arg);
	}

	if (rc < -1) {
		const char *opt, *alias, *reason;

		opt = poptBadOption (ctx, 0);
		alias = poptBadOption (ctx, POPT_BADOPTION_NOALIAS);
		reason = poptStrerror (rc);

		/* poptBadOption() with POPT_BADOPTION_NOALIAS fails to
		 * return the correct option if poptStuffArgs() was used. */
		if (!strcmp (opt, alias))
			fatal ("%s: %s", opt, reason);
		else
			fatal ("%s (aliased by %s): %s", opt, alias, reason);
	}
}

int main (int argc, const char *argv[])
{
	assert (argc >= 0);
	assert (argv != NULL);
	assert (argv[argc] == NULL);

	logit ("This is AMOC (version %s)", PACKAGE_VERSION);

	files_init ();

	memset (&params, 0, sizeof(params));
	params.allow_iface = 1;

	/* set locale according to the environment variables */
	if (!setlocale(LC_ALL, ""))
		logit ("Could not set locale!");

	poptContext ctx = poptGetContext ("amoc", argc, argv, mocp_opts, 0);
	process_options (ctx);

	if (params.foreground) params.only_server = 1;

	const char **rest = poptGetArgs (ctx);
	strings args;
	if (rest) args = unpack(rest);

	poptFreeContext (ctx);

	if (!params.allow_iface && params.only_server)
		fatal ("Server command options can't be used with --server!");

	options::load(params.allow_iface ? GUI : CLI);
	check_moc_dir();

	io_init ();
	decoder_init ();
	srand (time(NULL));

	if (params.allow_iface)
		start_moc (&params, args);
	else
		server_command (&params, args);

	decoder_cleanup ();
	io_cleanup ();
	files_cleanup ();

	return EXIT_SUCCESS;
}
