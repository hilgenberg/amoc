/*
 * MOC - music on console
 * Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#ifdef HAVE_GETRLIMIT
# include <sys/resource.h>
#endif
#include <sys/un.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <assert.h>

#include "../protocol.h"
#include "../output/audio.h"
#include "../output/oss.h"
#include "../options.h"
#include "server.h"
#include "../playlist.h"
#include "tags_cache.h"
#include "../files.h"
#include "../output/softmixer.h"
#include "../output/equalizer.h"
#include "ratings.h"

#define SERVER_LOG	"mocp_server_log"
#define PID_FILE	"pid"

struct client
{
	Socket *socket; 	/* NULL if inactive */
	int wants_plist_events;	/* requested playlist events? */
	pthread_mutex_t events_mtx;
	int requests_plist;	/* is the client waiting for the playlist? */
	int can_send_plist;	/* can this client send a playlist? */
	int lock;		/* is this client locking us? */
	int serial;		/* used for generating unique serial numbers */
};

struct Lock
{
	Lock(client *cli) : c(cli) { LOCK(c->events_mtx); assert(!c->socket->is_buffering()); }
	~Lock() { assert(!c->socket->is_buffering()); UNLOCK(c->events_mtx); }
	client *c;
};

static client clients[CLIENTS_MAX];

/* Thread ID of the server thread. */
static pthread_t server_tid;

/* Pipe used to wake up the server from select() from another thread. */
static int wake_up_pipe[2];

/* Socket used to accept incoming client connections. */
static int server_sock = -1;

/* Set to 1 when a signal arrived causing the program to exit. */
static volatile int server_quit = 0;

/* Information about currently played file */
static struct {
	int avg_bitrate;
	int bitrate;
	int rate;
	int channels;
} sound_info = {
	-1,
	-1,
	-1,
	-1
};

static tags_cache *tc;

extern char **environ;

static void write_pid_file ()
{
	char *fname = create_file_name (PID_FILE);
	FILE *file;

	if ((file = fopen(fname, "w")) == NULL)
		fatal ("Can't open pid file for writing: %s", xstrerror (errno));
	fprintf (file, "%d\n", getpid());
	fclose (file);
}

/* Check if there is a pid file and if it is valid, return the pid, else 0 */
static pid_t check_pid_file ()
{
	FILE *file;
	pid_t pid;
	char *fname = create_file_name (PID_FILE);

	/* Read the pid file */
	if ((file = fopen(fname, "r")) == NULL)
		return 0;
	if (fscanf(file, "%d", &pid) != 1) {
		fclose (file);
		return 0;
	}
	fclose (file);

	return pid;
}

static void sig_chld (int sig LOGIT_ONLY)
{
	int saved_errno;
	pid_t rc;

	log_signal (sig);

	saved_errno = errno;
	do {
		rc = waitpid (-1, NULL, WNOHANG);
	} while (rc > 0);
	errno = saved_errno;
}

static void sig_exit (int sig)
{
	log_signal (sig);
	server_quit = 1;

	// FIXME (JCF): pthread_*() are not async-signal-safe and
	//              should not be used within signal handlers.
	if (!pthread_equal (server_tid, pthread_self()))
		pthread_kill (server_tid, sig);
}

static void clients_init ()
{
	for (int i = 0; i < CLIENTS_MAX; i++) {
		clients[i].socket = NULL;
		pthread_mutex_init (&clients[i].events_mtx, NULL);
	}
}

static void clients_cleanup ()
{
	for (int i = 0; i < CLIENTS_MAX; i++) {
		delete clients[i].socket; clients[i].socket = NULL;
		int rc = pthread_mutex_destroy (&clients[i].events_mtx);
		if (rc != 0)
			log_errno ("Can't destroy events mutex", rc);
	}
}

/* Add a client to the list, return 1 if ok, 0 on error (max clients exceeded) */
static bool add_client (int sock)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (clients[i].socket) continue;
		clients[i].wants_plist_events = 0;
		clients[i].socket = new Socket(sock, false);
		clients[i].requests_plist = 0;
		clients[i].can_send_plist = 0;
		clients[i].lock = 0;
		tc->clear_queue(i);
		return true;
	}

	return false;
}

/* Return index of a client that has a lock acquired. Return -1 if there is no
 * lock. */
static int locking_client ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket && clients[i].lock)
			return i;
	return -1;
}

/* Acquire a lock for this client. Return 0 on error. */
static int client_lock (struct client *cli)
{
	if (cli->lock) {
		logit ("Client wants deadlock");
		return 0;
	}

	assert (locking_client() == -1);

	cli->lock = 1;
	logit ("Lock acquired for client with fd %d", cli->socket->fd());

	return 1;
}

/* Return != 0 if this client holds a lock. */
static int is_locking (const struct client *cli)
{
	return cli->lock;
}

/* Release the lock hold by the client. Return 0 on error. */
static int client_unlock (struct client *cli)
{
	if (!cli->lock) {
		logit ("Client wants to unlock when there is no lock");
		return 0;
	}

	cli->lock = 0;
	logit ("Lock released by client with fd %d", cli->socket->fd());
	return 1;
}

/* Return the client index from the clients table. */
static int client_index (const client &cli)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		const client &c = clients[i];
		if (&c == &cli) return i;
		if (c.socket->fd() == cli.socket->fd()) return i;
	}
	return -1;
}

static void del_client (client &cli)
{
	delete cli.socket; cli.socket = NULL;
	LOCK (cli.events_mtx);
	tc->clear_queue (client_index(cli));
	UNLOCK (cli.events_mtx);
}

/* Check if the process with given PID exists. Return != 0 if so. */
static bool valid_pid (const pid_t pid)
{
	return kill(pid, 0) == 0;
}

static void wake_up_server ()
{
	int w = 1;

	debug ("Waking up the server");

	if (write(wake_up_pipe[1], &w, sizeof(w)) < 0)
		log_errno ("Can't wake up the server: (write() failed)", errno);
}

static void redirect_output (FILE *stream)
{
	FILE *rc;

	if (stream == stdin)
		rc = freopen ("/dev/null", "r", stream);
	else
		rc = freopen ("/dev/null", "w", stream);

	if (!rc)
		fatal ("Can't open /dev/null: %s", xstrerror (errno));
}

static void log_process_stack_size ()
{
#if !defined(NDEBUG) && defined(HAVE_GETRLIMIT)
	int rc;
	struct rlimit limits;

	rc = getrlimit (RLIMIT_STACK, &limits);
	if (rc == 0)
		logit ("Process's stack size: %u", (unsigned int)limits.rlim_cur);
#endif
}

static void log_pthread_stack_size ()
{
#if !defined(NDEBUG) && defined(HAVE_PTHREAD_ATTR_GETSTACKSIZE)
	int rc;
	size_t stack_size;
	pthread_attr_t attr;

	rc = pthread_attr_init (&attr);
	if (rc)
		return;

	rc = pthread_attr_getstacksize (&attr, &stack_size);
	if (rc == 0)
		logit ("PThread's stack size: %u", (unsigned int)stack_size);

	pthread_attr_destroy (&attr);
#endif
}

/* Initialize the server - return fd of the listening socket or -1 on error */
void server_init (int debugging, int foreground)
{
	struct sockaddr_un sock_name;
	pid_t pid;

	logit ("Starting MOC Server");

	assert (server_sock == -1);

	pid = check_pid_file ();
	if (pid && valid_pid(pid)) {
		fprintf (stderr, "\nIt seems that the server is already running"
				" with pid %d.\n", pid);
		fprintf (stderr, "If it is not true, remove the pid file (%s)"
				" and try again.\n",
				create_file_name(PID_FILE));
		fatal ("Exiting!");
	}

	if (foreground)
		log_init_stream (stdout, "stdout");
	else {
		FILE *logfp;

		logfp = NULL;
		if (debugging) {
			logfp = fopen (SERVER_LOG, "a");
			if (!logfp)
				fatal ("Can't open server log file: %s", xstrerror (errno));
		}
		log_init_stream (logfp, SERVER_LOG);
	}

	if (pipe(wake_up_pipe) < 0)
		fatal ("pipe() failed: %s", xstrerror (errno));

	unlink (socket_name());

	/* Create a socket.
	 * For reasons why AF_UNIX is the correct constant to use in both
	 * cases, see the commentary the SVN log for commit r9999. */
	server_sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (server_sock == -1)
		fatal ("Can't create socket: %s", xstrerror (errno));
	sock_name.sun_family = AF_UNIX;
	strcpy (sock_name.sun_path, socket_name());

	/* Bind to socket */
	if (bind(server_sock, (struct sockaddr *)&sock_name, SUN_LEN(&sock_name)) == -1)
		fatal ("Can't bind() to the socket: %s", xstrerror (errno));

	if (listen(server_sock, 1) == -1)
		fatal ("listen() failed: %s", xstrerror (errno));

	/* Log stack sizes so stack overflows can be debugged. */
	log_process_stack_size ();
	log_pthread_stack_size ();

	clients_init ();
	audio_initialize ();
	tc = new tags_cache();
	tc->load (create_file_name("cache"));

	server_tid = pthread_self ();
	xsignal (SIGTERM, sig_exit);
	xsignal (SIGINT, foreground ? sig_exit : SIG_IGN);
	xsignal (SIGHUP, SIG_IGN);
	xsignal (SIGQUIT, sig_exit);
	xsignal (SIGPIPE, SIG_IGN);
	xsignal (SIGCHLD, sig_chld);

	write_pid_file ();

	if (!foreground) {
		setsid ();
		redirect_output (stdin);
		redirect_output (stdout);
		redirect_output (stderr);
	}

	return;
}

static bool send_data_int (client *cli, int data)
{
	Lock lock(cli);
	return cli->socket->send((int)EV_DATA) && cli->socket->send(data);
}
static bool send_data_bool (client *cli, bool data)
{
	Lock lock(cli);
	return cli->socket->send((int)EV_DATA) && cli->socket->send(data);
}
static bool send_data_str (client *cli, const char *str)
{
	Lock lock(cli);
	return cli->socket->send((int)EV_DATA) && cli->socket->send(str);
}

/* Add event to the client's queue */
void add_event (client &cli, int type)
{
	Lock lock(&cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.finish();
}template<typename T> void add_event (client &cli, int type, T data)
{
	Lock lock(&cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.send(data);
	sock.finish();
}
template<typename T1, typename T2> void add_event (client &cli, int type, T1 d1, T2 d2)
{
	Lock lock(&cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.send(d1);
	sock.send(d2);
	sock.finish();
}

/* Return true iff 'event' is a playlist event. */
static inline bool is_plist_event (const int event)
{
	switch (event) {
	case EV_PLIST_ADD:
	case EV_PLIST_DEL:
	case EV_PLIST_MOVE:
	case EV_PLIST_CLEAR:
		return true;
	}
	return false;
}

template<typename T1, typename T2> void add_event_all(int type, T1 d1, T2 d2)
{
	return;

	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		if (!clients[i].wants_plist_events && is_plist_event(type)) continue;
		add_event(clients[i], type, d1, d2);
		added = true;
	}

	if (added)
		wake_up_server ();
	else
		debug ("No events have been added because there are no clients");
}
template<typename T1> void add_event_all(int type, T1 d1)
{
		return;

	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		if (!clients[i].wants_plist_events && is_plist_event(type)) continue;
		add_event(clients[i], type, d1);
		added = true;
	}

	if (added)
		wake_up_server ();
	else
		debug ("No events have been added because there are no clients");
}
void add_event_all(int type)
{
	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		if (!clients[i].wants_plist_events && is_plist_event(type)) continue;
		add_event(clients[i], type);
		added = true;
	}

	if (added)
		wake_up_server ();
	else
		debug ("No events have been added because there are no clients");
}

/* Send events from the queue. Return 0 on error. */
static bool flush_events (client &cli)
{
	if (!cli.socket) return false;
	Lock lock(&cli);
	auto &sock = *cli.socket;
	while (!sock.packets.empty() && sock.send_next_packet_noblock() == 1) ;
	return true;
}

/* Send events to clients whose sockets are ready to write. */
static void send_events (fd_set *fds)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket
				&& FD_ISSET(clients[i].socket->fd(), fds)) {
			debug ("Flushing events for client %d", i);
			if (!flush_events (clients[i])) {
				close (clients[i].socket->fd());
				del_client (clients[i]);
			}
		}
}

/* End playing and cleanup. */
static void server_shutdown ()
{
	logit ("Server exiting...");
	audio_exit ();
	delete tc; tc = NULL;
	unlink (socket_name());
	unlink (create_file_name(PID_FILE));
	close (wake_up_pipe[0]);
	close (wake_up_pipe[1]);
	logit ("Server exited");
	log_close ();
}

/* Report an error logging it and sending a message to the client. */
void server_error (const char *file, int line, const char *function, const char *msg)
{
	internal_logit (file, line, function, "ERROR: %s", msg);
	add_event_all(EV_SRV_ERROR, msg);
}

/* Handle CMD_SET_RATING, return 1 if ok or 0 on error. */
static bool req_set_rating (client &cli)
{
	str file;
	int rating;

	if (!cli.socket->get(file) || !cli.socket->get(rating)) return false;
	if (file.empty())
	{
		char *cur = audio_get_sname();
		if (cur && *cur) file = cur;
		free(cur);
		if (file.empty()) return false;
	}

	logit ("Rating %s %d/5", file.c_str(), rating);
	
	if (ratings_write_file (file.c_str(), rating))
	{
		tc->ratings_changed(file.c_str(), rating);
		add_event_all (EV_FILE_RATING, file, rating);
	}

	return true;
}

/* Handle CMD_JUMP_TO, return 1 if ok or 0 on error */
static bool req_jump_to (client &cli)
{
	int sec;
	if (!cli.socket->get(sec)) return false;

	if (sec < 0)
	{
		sec = -sec; /* percentage */
		assert(sec >= 0 && sec <= 100);

		char *file = audio_get_sname ();
		if (!file || !*file)
		{
			free (file);
			return false;
		}
	
		struct file_tags tags = tc->get_immediate (file);
		free (file);

		if (tags.time <= 0) return false;
		sec = (tags.time * sec)/100;
	}

	logit ("Jumping to %ds", sec);
	audio_jump_to (sec);
	return true;
}

/* Handle CMD_GET_PLIST. Return 0 on error. */
static int get_client_plist (struct client *cli)
{
	debug ("Client with fd %d requests the playlist", cli->socket);

	/* Find the first connected client, and ask it to send the playlist.
	 * Here, send 1 if there is a client with the playlist, or 0 if there
	 * isn't. */

	cli->requests_plist = 1;

	int first = -1;
	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket || !clients[i].can_send_plist) continue;
		first = i;
		break;
	}

	if (first == -1) {
		debug ("No clients with the playlist");
		cli->requests_plist = 0;
		if (!send_data_int(cli, 0))
			return 0;
		return 1;
	}

	if (!send_data_int(cli, 1))
		return 0;

	if (!clients[first].socket->send(EV_SEND_PLIST))
		return 0;

	return 1;
}

/* Handle CMD_SEND_PLIST. Some client requested to get the playlist, so we asked
 * another client to send it (EV_SEND_PLIST). */
static int req_send_plist (struct client *cli)
{
	int requesting = -1;
	for (int i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].requests_plist) { requesting = i; break; }

	Socket *send_fd = NULL;
	int serial;

	debug ("Client with fd %d wants to send its playlists", cli->socket);

	if (requesting == -1) {
		logit ("No clients are requesting the playlist");
	}
	else {
		send_fd = clients[requesting].socket;
		if (!send_fd->send(EV_DATA)) {
			logit ("Error while sending response; disconnecting the client");
			close (send_fd->fd());
			del_client (clients[requesting]);
			send_fd = NULL;
		}
	}

	if (!cli->socket->get(serial)) {
		logit ("Error while getting serial");
		return 0;
	}

	if (send_fd && !send_fd->send(serial)) {
		error ("Error while sending serial; disconnecting the client");
		close (send_fd->fd());
		del_client (clients[requesting]);
		send_fd = NULL;
	}

	/* Even if no clients are requesting the playlist, we must read it,
	 * because there is no way to say that we don't need it. */
	str item;
	while (cli->socket->get(item) && !item.empty()) {
		if (send_fd && !send_fd->send(item)) {
			logit ("Error while sending item; disconnecting the client");
			close (send_fd->fd());
			del_client (clients[requesting]);
			send_fd = NULL;
		}
	}

	if (send_fd && send_fd->send("")) {
		logit ("Error while sending end of playlist mark; "
		       "disconnecting the client");
		close (send_fd->fd());
		del_client (clients[requesting]);
		return 0;
	}
	logit ("Playlist sent");

	if (requesting != -1)
		clients[requesting].requests_plist = 0;

	return true;
}

/* Handle command that synchronises the playlists between interfaces
 * (except forwarding the whole list). Return 0 on error. */
static int plist_sync_cmd (struct client &cli, const int cmd)
{
	if (cmd == CMD_CLI_PLIST_ADD) {
		debug ("Sending EV_PLIST_ADD");
		str item;
		if (!cli.socket->get(item)) {
			logit ("Error while receiving item");
			return 0;
		}
		add_event_all (EV_PLIST_ADD, item);
	}
	else if (cmd == CMD_CLI_PLIST_DEL) {
		str file;

		debug ("Sending EV_PLIST_DEL");

		if (!cli.socket->get(file)) {
			logit ("Error while receiving file");
			return 0;
		}

		add_event_all (EV_PLIST_DEL, file);
	}
	else if (cmd == CMD_CLI_PLIST_MOVE) {
		int i, j;
		if (!cli.socket->get(i) || !cli.socket->get(j)) {
			logit ("Error while receiving file");
			return 0;
		}
		add_event_all (EV_PLIST_MOVE, i, j);

	}
	else { /* it can be only CMD_CLI_PLIST_CLEAR */
		debug ("Sending EV_PLIST_CLEAR");
		add_event_all (EV_PLIST_CLEAR);
	}

	return 1;
}

/* Generate a unique playlist serial number. */
static int gen_serial (const client *cli)
{
	static int seed = 0;
	int serial;

	/* Each client must always get a different serial number, so we use
	 * also the client index to generate it. It must also not be used by
	 * our playlist to not confuse clients.
	 * There can be 256 different serial number per client, but it's
	 * enough since clients use only two playlists. */

	do {
		serial = (seed << 8) | client_index(*cli);
		seed = (seed + 1) & 0xFF;
	} while (serial == audio_plist_get_serial());

	debug ("Generated serial %d for client with fd %d", serial, cli->socket);

	return serial;
}

/* Send tags to the client. Return 0 on error. */
static int req_get_tags (client *cli)
{
	file_tags *tags;
	int res = 1;

	debug ("Sending tags to client with fd %d...", cli->socket->fd());

	if (!cli->socket->send(EV_DATA)) {
		logit ("Error when sending EV_DATA");
		return 0;
	}

	tags = audio_get_curr_tags ();
	if (!cli->socket->send(tags)) {
		logit ("Error when sending tags");
		res = 0;
	}
	delete tags;

	return res;
}

void update_eq_name()
{
	char buffer[27];

	char *n = equalizer_current_eqname();

	int l = strlen(n);

	/* Status message can only take strings up to 25 chars
	 * (Without terminating zero).
	 * The message header has 11 chars (EQ set to...).
	 */
	if (l > 14)
	{
		n[14] = 0;
		n[13] = '.';
		n[12] = '.';
		n[11] = '.';
	}

	sprintf(buffer, "EQ set to: %s", n);

	logit("%s", buffer);

	free(n);

	status_msg(buffer);
}

void req_toggle_equalizer ()
{
	equalizer_set_active(!equalizer_is_active());

	update_eq_name();
}

void req_equalizer_refresh()
{
	equalizer_refresh();

	status_msg("Equalizer refreshed");

	logit("Equalizer refreshed");
}

void req_equalizer_prev()
{
	equalizer_prev();

	update_eq_name();
}

void req_equalizer_next()
{
	equalizer_next();

	update_eq_name();
}

void req_toggle_make_mono()
{
	char buffer[128];

	softmixer_set_mono(!softmixer_is_mono());

	sprintf(buffer, "Mono-Mixing set to: %s", softmixer_is_mono()?"on":"off");

	status_msg(buffer);
}

/* Handle CMD_GET_FILE_TAGS. Return 0 on error. */
static int get_file_tags (const int cli_id)
{
	str file;
	if (!clients[cli_id].socket->get(file))
		return 0;

	tc->add_request (file.c_str(), cli_id);

	return 1;
}

static int abort_tags_requests (const int cli_id)
{
	str file;
	if (!clients[cli_id].socket->get(file))
		return 0;

	tc->clear_up_to (file.c_str(), cli_id);

	return 1;
}

/* Receive a command from the client and execute it. */
static void handle_command (const int client_id)
{
	int cmd;
	int err = 0;
	client &cli = clients[client_id];

	if (!cli.socket->get(cmd)) {
		logit ("Failed to get command from the client");
		close (cli.socket->fd());
		del_client (cli);
		return;
	}

	switch (cmd) {
		case CMD_QUIT:
			logit ("Exit request from the client");
			close (cli.socket->fd());
			del_client (cli);
			server_quit = 1;
			break;
		case CMD_LIST_CLEAR:
			logit ("Clearing the list");
			audio_plist_clear ();
			break;
		case CMD_LIST_ADD:
		{
			str file;
			err = !cli.socket->get(file);
			if (!err)
			{
				logit ("Adding '%s' to the list", file.c_str());
				audio_plist_add (file.c_str());
			}
			break;
		}
		case CMD_PLAY:
		{
			str file;
			err = !cli.socket->get(file);
			if (!err)
			{
				logit ("Playing %s", file.empty() ? "first element on the list" : file.c_str());
				audio_play (file.c_str());
			}
			break;
		}
		case CMD_DISCONNECT:
			logit ("Client disconnected");
			close (cli.socket->fd());
			del_client (cli);
			break;
		case CMD_PAUSE:
			audio_pause ();
			break;
		case CMD_UNPAUSE:
			audio_unpause ();
			break;
		case CMD_STOP:
			audio_stop ();
			break;
		case CMD_GET_CTIME:
			if (!send_data_int(&cli, MAX(0, audio_get_time())))
				err = 1;
			break;
		case CMD_SEEK:
		{
			int sec;
			err = !cli.socket->get(sec);
			if (!err)
			{
				logit ("Seeking %ds", sec);
				audio_seek (sec);
			}
			break;
		}
		case CMD_JUMP_TO:
			if (!req_jump_to(cli))
				err = 1;
			break;
		case CMD_GET_SNAME:
		{
			char *s = audio_get_sname ();
			err = !cli.socket->send(s);
			free (s);
			break;
		}
		case CMD_GET_STATE:
			err = !send_data_int(&cli, audio_get_state());
			break;
		case CMD_GET_BITRATE:
			err = !send_data_int(&cli, sound_info.bitrate);
			break;
		case CMD_GET_AVG_BITRATE:
			err = !send_data_int(&cli, sound_info.avg_bitrate);
			break;
		case CMD_GET_RATE:
			err = !send_data_int(&cli, sound_info.rate);
			break;
		case CMD_GET_CHANNELS:
			err = !send_data_int(&cli, sound_info.channels);
			break;
		case CMD_NEXT:
			audio_next ();
			break;
		case CMD_PREV:
			audio_prev ();
			break;
		case CMD_PING:
			err = !cli.socket->send(EV_PONG);
			break;
		case CMD_GET_OPTION_AUTONEXT:
			if (!send_data_bool(&cli, options::AutoNext))
				err = 1;
			break;
		case CMD_GET_OPTION_SHUFFLE:
			if (!send_data_bool(&cli, options::Shuffle))
				err = 1;
			break;
		case CMD_GET_OPTION_REPEAT:
			if (!send_data_bool(&cli, options::Repeat))
				err = 1;
			break;
		case CMD_SET_OPTION_AUTONEXT:
		{
			int val = 0;
			if (!cli.socket->get(val))
			{
				err = 1;
			}
			else
			{
				options::AutoNext = val;
				add_event_all (EV_OPTIONS);
			}
			break;
		}
		case CMD_SET_OPTION_SHUFFLE:
		{
			int val = 0;
			if (!cli.socket->get(val))
			{
				err = 1;
			}
			else
			{
				options::Shuffle = val;
				add_event_all (EV_OPTIONS);
			}
			break;
		}
		case CMD_SET_OPTION_REPEAT:
		{
			int val = 0;
			if (!cli.socket->get(val))
			{
				err = 1;
			}
			else
			{
				options::Repeat = val;
				add_event_all (EV_OPTIONS);
			}
			break;
		}
		case CMD_GET_MIXER:
			if (!send_data_int(&cli, audio_get_mixer()))
				err = 1;
			break;
		case CMD_SET_MIXER:
		{
			int val;
			err = !cli.socket->get(val);
			if (!err) audio_set_mixer (val);
			break;
		}
		case CMD_DELETE:
		{
			str file;
			err = !cli.socket->get(file);
			if (!err) debug ("Request for deleting %s", file.c_str());
			if (!err) audio_plist_delete (file.c_str());
			break;
		}
		case CMD_SEND_PLIST_EVENTS:
			cli.wants_plist_events = 1;
			logit ("Request for events");
			break;
		case CMD_GET_PLIST:
			err = !get_client_plist(&cli);
			break;
		case CMD_SEND_PLIST:
			if (!req_send_plist(&cli))
				err = 1;
			break;
		case CMD_CAN_SEND_PLIST:
			cli.can_send_plist = 1;
			break;
		case CMD_CLI_PLIST_ADD:
		case CMD_CLI_PLIST_DEL:
		case CMD_CLI_PLIST_CLEAR:
		case CMD_CLI_PLIST_MOVE:
			if (!plist_sync_cmd(cli, cmd))
				err = 1;
			break;
		case CMD_LOCK:
			if (!client_lock(&cli))
				err = 1;
			break;
		case CMD_UNLOCK:
			if (!client_unlock(&cli))
				err = 1;
			break;
		case CMD_GET_SERIAL:
			err = !send_data_int(&cli, gen_serial(&cli));
			break;
		case CMD_PLIST_GET_SERIAL:
			err = !send_data_int(&cli, audio_plist_get_serial());
			break;
		case CMD_PLIST_SET_SERIAL:
		{
			int serial;
			err = !cli.socket->get(serial);
			if (!err && serial < 0)
			{
				logit ("Client wants to set bad serial number");
				err = 1;
			}
			if (!err)
			{
				debug ("Setting the playlist serial number to %d", serial);
				audio_plist_set_serial (serial);
			}
			break;
		}
		case CMD_GET_TAGS:
			if (!req_get_tags(&cli))
				err = 1;
			break;
		case CMD_TOGGLE_MIXER_CHANNEL:
			audio_toggle_mixer_channel ();
			add_event_all (EV_MIXER_CHANGE);
			break;
		case CMD_TOGGLE_SOFTMIXER:
			softmixer_set_active(!softmixer_is_active());
			add_event_all (EV_MIXER_CHANGE);
			break;
		case CMD_GET_MIXER_CHANNEL_NAME:
		{
			char *name = audio_get_mixer_channel_name ();
			err = !send_data_str(&cli, name);
			free (name);
			break;
		}
		case CMD_GET_FILE_TAGS:
			if (!get_file_tags(client_id))
				err = 1;
			break;
		case CMD_ABORT_TAGS_REQUESTS:
			if (!abort_tags_requests(client_id))
				err = 1;
			break;
		case CMD_LIST_MOVE:
		{
			int i, j;
			err = !cli.socket->get(i) || !cli.socket->get(j);
			if (!err) audio_plist_move (i, j);
			break;
		}
		case CMD_TOGGLE_EQUALIZER:
			req_toggle_equalizer();
			break;
		case CMD_EQUALIZER_REFRESH:
			req_equalizer_refresh();
			break;
		case CMD_EQUALIZER_PREV:
			req_equalizer_prev();
			break;
		case CMD_EQUALIZER_NEXT:
			req_equalizer_next();
			break;
		case CMD_TOGGLE_MAKE_MONO:
			req_toggle_make_mono();
			break;
		case CMD_SET_RATING:
			if (!req_set_rating(cli))
				err = 1;
			break;
		default:
			logit ("Bad command (0x%x) from the client", cmd);
			err = 1;
	}

	if (err) {
		logit ("Closing client connection due to error");
		close (cli.socket->fd());
		del_client (cli);
	}
}

/* Add clients file descriptors to fds. */
static void add_clients_fds (fd_set *read, fd_set *write)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket) {
			if (locking_client() == -1 || is_locking(&clients[i]))
				FD_SET (clients[i].socket->fd(), read);

			LOCK (clients[i].events_mtx);
			if (!clients[i].socket->packets.empty())
				FD_SET (clients[i].socket->fd(), write);
			UNLOCK (clients[i].events_mtx);
		}
}

/* Return the maximum fd from clients and the argument. */
static int max_fd (int max)
{
	int i;

	if (wake_up_pipe[0] > max)
		max = wake_up_pipe[0];

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket && clients[i].socket->fd() > max)
			max = clients[i].socket->fd();
	return max;
}

/* Handle clients whose fds are ready to read. */
static void handle_clients (fd_set *fds)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket
				&& FD_ISSET(clients[i].socket->fd(), fds)) {
			if (locking_client() == -1
					|| is_locking(&clients[i]))
				handle_command (i);
			else
				debug ("Not getting a command from client with"
						" fd %d because of lock",
						clients[i].socket->fd());
		}
}

/* Close all client connections sending EV_EXIT. */
static void close_clients ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket) {
			clients[i].socket->send(EV_EXIT);
			close (clients[i].socket->fd());
			del_client (clients[i]);
		}
}

/* Handle incoming connections */
void server_loop ()
{
	struct sockaddr_un client_name;
	socklen_t name_len = sizeof (client_name);

	logit ("MOC server started, pid: %d", getpid());

	assert (server_sock != -1);

	do {
		int res;
		fd_set fds_write, fds_read;

		FD_ZERO (&fds_read);
		FD_ZERO (&fds_write);
		FD_SET (server_sock, &fds_read);
		FD_SET (wake_up_pipe[0], &fds_read);
		add_clients_fds (&fds_read, &fds_write);

		res = 0;
		if (!server_quit)
			res = select (max_fd(server_sock)+1, &fds_read,
					&fds_write, NULL, NULL);

		if (res == -1 && errno != EINTR && !server_quit)
			fatal ("select() failed: %s", xstrerror (errno));

		if (!server_quit && res >= 0) {
			if (FD_ISSET(server_sock, &fds_read)) {
				int client_sock;

				debug ("accept()ing connection...");
				client_sock = accept (server_sock,
					(struct sockaddr *)&client_name,
					&name_len);

				if (client_sock == -1)
					fatal ("accept() failed: %s", xstrerror (errno));
				logit ("Incoming connection");
				if (!add_client(client_sock))
				{
					logit ("Closing connection due to maximum number of clients reached");
					int t = EV_BUSY;
					::send(client_sock, &t, sizeof(int), 0);
					close (client_sock);
				}
			}

			if (FD_ISSET(wake_up_pipe[0], &fds_read)) {
				int w;

				logit ("Got 'wake up'");

				if (read(wake_up_pipe[0], &w, sizeof(w)) < 0)
					fatal ("Can't read wake up signal: %s", xstrerror (errno));
			}

			send_events (&fds_write);
			handle_clients (&fds_read);
		}

		if (server_quit)
			logit ("Exiting...");

	} while (!server_quit);

	close_clients ();
	clients_cleanup ();
	close (server_sock);
	server_sock = -1;
	server_shutdown ();
}

void set_info_bitrate (const int bitrate)
{
	sound_info.bitrate = bitrate;
	add_event_all (EV_BITRATE);
}

void set_info_channels (const int channels)
{
	sound_info.channels = channels;
	add_event_all (EV_CHANNELS);
}

void set_info_rate (const int rate)
{
	sound_info.rate = rate;
	add_event_all (EV_RATE);
}

void set_info_avg_bitrate (const int avg_bitrate)
{
	sound_info.avg_bitrate = avg_bitrate;
	add_event_all (EV_AVG_BITRATE);
}

/* Notify the client about change of the player state. */
void state_change ()
{
	add_event_all (EV_STATE);
}

void ctime_change ()
{
	add_event_all (EV_CTIME);
}

void tags_change ()
{
	add_event_all (EV_TAGS);
}

void status_msg (const char *msg)
{
	add_event_all (EV_STATUS_MSG, msg);
}

void tags_response (const int client_id, const char *file, const file_tags *tags)
{
	logit("sending tag response");
	assert (file != NULL);
	assert (tags != NULL);
	assert (LIMIT(client_id, CLIENTS_MAX));

	if (clients[client_id].socket) {
		add_event (clients[client_id], EV_FILE_TAGS, file, tags);
		wake_up_server ();
	}
}

void ev_audio_start ()
{
	add_event_all (EV_AUDIO_START);
}

void ev_audio_stop ()
{
	add_event_all (EV_AUDIO_STOP);
}
