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
#include "protocol.h"
#include "rcc.h"

static int mocp_argc;
static const char **mocp_argv;
static int popt_next_val = 1;

/* List of MOC-specific environment variables. */
static struct {
	const char *name;
	const char *desc;
} environment_variables[] = {
	{"MOCP_OPTS", "Additional command line options"},
	{"MOCP_POPTRC", "List of POPT configuration files"}
};

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
	int new_rating;
};

/* Connect to the server, return fd of the socket or -1 on error. */
static int server_connect ()
{
	struct sockaddr_un sock_name;
	int sock;

	/* Create a socket.
	 * For reasons why AF_UNIX is the correct constant to use in both
	 * cases, see the commentary the SVN log for commit r2833. */
	if ((sock = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
		 return -1;

	sock_name.sun_family = AF_UNIX;
	strcpy (sock_name.sun_path, socket_name());

	if (connect(sock, (struct sockaddr *)&sock_name,
				SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Ping the server.
 * Return 1 if the server responds with EV_PONG, otherwise 0. */
static int ping_server (Socket &srv)
{
	int event;

	srv.send(CMD_PING); /* ignore errors - the server could have
				     already closed the connection and sent
				     EV_BUSY */
	if (!srv.get(event))
		fatal ("Error when receiving pong response!");
	return event == EV_PONG ? 1 : 0;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	char *dir_name = create_file_name ("");
	struct stat file_stat;

	/* strip trailing slash */
	dir_name[strlen(dir_name)-1] = 0;

	if (stat (dir_name, &file_stat) == -1) {
		if (errno != ENOENT)
			fatal ("Error trying to check for " CONFIG_DIR " directory: %s",
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
static void start_moc (const struct parameters *params, stringlist &args)
{
	int server_sock;

	if (params->foreground) {
		set_me_server ();
		server_init (params->debug, params->foreground);
		server_loop ();
		return;
	}

	server_sock = server_connect ();

	if (server_sock != -1 && params->only_server)
		fatal ("Server is already running!");

	if (server_sock == -1) {
		int i = 0;
		int notify_pipe[2];
		ssize_t rc;

		printf ("Running the server...\n");

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", xstrerror (errno));

		switch (fork()) {
		case 0: /* child - start server */
			set_me_server ();
			server_init (params->debug, params->foreground);
			rc = write (notify_pipe[1], &i, sizeof(i));
			if (rc < 0)
				fatal ("write() to notify pipe failed: %s", xstrerror (errno));
			close (notify_pipe[0]);
			close (notify_pipe[1]);
			server_loop ();
			decoder_cleanup ();
			io_cleanup ();
			files_cleanup ();
			rcc_cleanup ();
			common_cleanup ();
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

	Socket srv(server_sock, false);
	if (params->only_server)
		srv.send(CMD_DISCONNECT);
	else {
		xsignal (SIGPIPE, SIG_IGN);
		if (!ping_server (srv))
			fatal ("Can't connect to the server!");

		#define CLIENT_LOG	"mocp_client_log"

		FILE *logfp = NULL;
		if (params->debug) {
			logfp = fopen (CLIENT_LOG, "a");
			if (!logfp) fatal ("Can't open client log file: %s", xstrerror (errno));
		}
		log_init_stream (logfp, CLIENT_LOG);

		Client client(server_sock, args);
		client.run();
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

void interface_cmdline_playit (Socket &srv, stringlist &args)
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
static void server_command (struct parameters *params, stringlist &args)
{
	int srv_sock;

	if ((srv_sock = server_connect()) == -1)
		fatal ("The server is not running!");

	xsignal (SIGPIPE, SIG_IGN);
	Socket srv(srv_sock, true);
	if (!ping_server (srv))
		fatal ("Can't connect to the server!");


	if (params->playit)
		interface_cmdline_playit (srv, args);
	if (params->play)
		interface_cmdline_play_first (srv);
	if (params->rate)
		interface_cmdline_set_rating (srv, params->new_rating);
	if (params->exit) {
		if (!srv.send(CMD_QUIT))
			fatal ("Can't send command!");
	}
	else if (params->stop) {
		if (!srv.send(CMD_STOP) || !srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->pause) {
		if (!srv.send(CMD_PAUSE) || !srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->next) {
		if (!srv.send(CMD_NEXT) || !srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->previous) {
		if (!srv.send(CMD_PREV) || !srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->unpause) {
		if (!srv.send(CMD_UNPAUSE) || !srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->toggle_pause) {
		int state, ev, cmd = -1;

		if (!srv.send(CMD_GET_STATE))
			fatal ("Can't send commands!");
		if (!srv.get(ev) || ev != EV_DATA || !srv.get(state))
			fatal ("Can't get data from the server!");

		if (state == STATE_PAUSE)
			cmd = CMD_UNPAUSE;
		else if (state == STATE_PLAY)
			cmd = CMD_PAUSE;

		if (cmd != -1 && !srv.send(cmd))
			fatal ("Can't send commands!");
		if (!srv.send(CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}

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

	/* Show compiled-in components */
	printf ("    Compiled with :");
#ifdef HAVE_OSS
	printf (" OSS");
#endif
#ifdef HAVE_SNDIO
	printf (" SNDIO");
#endif
#ifdef HAVE_ALSA
	printf (" ALSA");
#endif
#ifdef HAVE_JACK
	printf (" JACK");
#endif
#ifndef NDEBUG
	printf (" DEBUG");
#endif
#ifdef HAVE_CURL
	printf (" Network streams");
#endif
#ifdef HAVE_SAMPLERATE
	printf (" resample");
#endif
	putchar ('\n');

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
	size_t ix;

	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintHelp (ctx, stdout, 0);

	printf ("\nEnvironment variables:\n\n");
	for (ix = 0; ix < ARRAY_SIZE(environment_variables); ix += 1)
		printf ("  %-34s%s\n", environment_variables[ix].name,
		                       environment_variables[ix].desc);
	printf ("\n");
}

/* Disambiguate the user's request. */
static void show_misc_cb (poptContext ctx,
                          enum poptCallbackReason unused1 ATTR_UNUSED,
                          const struct poptOption *opt,
                          const char *unused2 ATTR_UNUSED,
                          void *unused3 ATTR_UNUSED)
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
	{"rate", '*', POPT_ARG_INT, &params.new_rating, CL_RATE,
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

/* Check that the ~/.popt file is secure. */
static void check_popt_secure ()
{
	int len;
	const char *home, dot_popt[] = ".popt";
	char *home_popt;

	home = get_home ();
	len = strlen (home) + strlen (dot_popt) + 2;
	home_popt = (char*) xcalloc (len, sizeof (char));
	snprintf (home_popt, len, "%s/%s", home, dot_popt);
	if (!is_secure (home_popt))
		fatal ("POPT config file is not secure: %s", home_popt);
	free (home_popt);
}

/* Read the default POPT configuration file. */
static void read_default_poptrc (poptContext ctx)
{
	int rc;

	check_popt_secure ();
	rc = poptReadDefaultConfig (ctx, 0);

	if (rc == POPT_ERROR_ERRNO) {
		int saved_errno = errno;

		fprintf (stderr, "\n"
		         "WARNING: The following fatal error message may be bogus!\n"
		         "         If you have an empty /etc/popt.d directory, try\n"
		         "         adding an empty file to it.  If that does not fix\n"
		         "         the problem then you have a genuine error.\n");

		errno = saved_errno;
	}

	if (rc != 0)
		fatal ("Error reading default POPT config file: %s",
		        poptStrerror (rc));
}

/* Read the POPT configuration files(s). */
static void read_popt_config (poptContext ctx)
{
	read_default_poptrc (ctx);
}

/* Process the command line options. */
static void process_options (poptContext ctx)
{
	int rc;

	while ((rc = poptGetNextOpt (ctx)) >= 0) {
		const char *jump_type, *arg;

		arg = poptGetOptArg (ctx);

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
		if (!strcmp (opt, alias) || getenv ("MOCP_OPTS"))
			fatal ("%s: %s", opt, reason);
		else
			fatal ("%s (aliased by %s): %s", opt, alias, reason);
	}
}

/* Process the command line options and arguments. */
static stringlist process_command_line()
{
	const char **rest;
	poptContext ctx;
	stringlist result;

	ctx = poptGetContext ("mocp", mocp_argc, mocp_argv, mocp_opts, 0);

	read_popt_config (ctx);
	process_options (ctx);

	if (params.foreground)
		params.only_server = 1;

	rest = poptGetArgs (ctx);
	if (rest)
		result = unpack(rest);

	poptFreeContext (ctx);

	return result;
}

int main (int argc, const char *argv[])
{
	assert (argc >= 0);
	assert (argv != NULL);
	assert (argv[argc] == NULL);

	mocp_argc = argc;
	mocp_argv = argv;

	logit ("This is Music On Console (version %s)", PACKAGE_VERSION);

	files_init ();

	if (get_home () == NULL)
		fatal ("Could not determine user's home directory!");

	memset (&params, 0, sizeof(params));
	params.allow_iface = 1;
	options::init (create_file_name ("config"));

	/* set locale according to the environment variables */
	if (!setlocale(LC_ALL, ""))
		logit ("Could not set locale!");

	stringlist args = process_command_line();

	if (!params.allow_iface && params.only_server)
		fatal ("Server command options can't be used with --server!");

	options::init ("config");

	check_moc_dir ();

	io_init ();
	rcc_init ();
	decoder_init (params.debug);
	srand (time(NULL));

	if (params.allow_iface)
		start_moc (&params, args);
	else
		server_command (&params, args);

	decoder_cleanup ();
	io_cleanup ();
	rcc_cleanup ();
	files_cleanup ();
	common_cleanup ();

	return EXIT_SUCCESS;
}
