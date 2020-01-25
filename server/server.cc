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

#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include "protocol.h"
#include "../Socket.h"
#include "audio.h"
#include "output/oss.h"
#include "server.h"
#include "../playlist.h"
#include "tags_cache.h"
#include "output/softmixer.h"
#include "output/equalizer.h"
#include "ratings.h"

#define SERVER_LOG	"amoc_server_log"
#define PID_FILE	"pid"
#define PLAYLIST_FILE	"playlist.m3u"

struct client
{
	Socket *socket; 	/* NULL if inactive */
	pthread_mutex_t events_mtx;
};
static client clients[CLIENTS_MAX];

// RAII lock for the client's event mutex
struct Lock
{
	Lock(client &cli) : c(cli) { LOCK(c.events_mtx); assert(!c.socket->is_buffering()); }
	~Lock() { assert(!c.socket->is_buffering()); UNLOCK(c.events_mtx); }
	client &c;
};


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
	str fname = options::run_file_path(PID_FILE);
	FILE *file = fopen(fname.c_str(), "w");

	if (!file) fatal ("Can't open pid file for writing: %s", xstrerror (errno));
	fprintf(file, "%d\n", getpid());
	fclose(file);
}

/* Check if there is a pid file and if it is valid, return the pid, else 0 */
static pid_t check_pid_file ()
{
	str fname = options::run_file_path(PID_FILE);
	FILE *file = fopen(fname.c_str(), "r");
	if (!file) return 0;

	/* Read the pid file */
	pid_t pid;
	if (fscanf(file, "%d", &pid) != 1) {
		fclose (file);
		return 0;
	}
	fclose (file);

	return pid;
}

static void sig_chld (int sig)
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
		if (rc != 0) log_errno ("Can't destroy events mutex", rc);
	}
}

/* Add a client to the list, return 1 if ok, 0 on error (max clients exceeded) */
static bool add_client (int sock)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (clients[i].socket) continue;
		clients[i].socket = new Socket(sock, false);
		tc->clear_queue(i);
		return true;
	}

	return false;
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
	int i = client_index(cli);
	LOCK (cli.events_mtx);
	close (cli.socket->fd());
	delete cli.socket; cli.socket = NULL;
	tc->clear_queue(i);
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
#if !defined(NDEBUG)
	int rc;
	struct rlimit limits;

	rc = getrlimit (RLIMIT_STACK, &limits);
	if (rc == 0)
		logit ("Process's stack size: %u", (unsigned int)limits.rlim_cur);
#endif
}

static void log_pthread_stack_size ()
{
#if !defined(NDEBUG)
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
				options::run_file_path(PID_FILE).c_str());
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

	unlink (options::SocketPath.c_str());

	/* Create a socket.
	 * For reasons why AF_UNIX is the correct constant to use in both
	 * cases, see the commentary the SVN log for commit r9999. */
	server_sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (server_sock == -1)
		fatal ("Can't create socket: %s", xstrerror (errno));
	sock_name.sun_family = AF_UNIX;
	strcpy (sock_name.sun_path, options::SocketPath.c_str());

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
	tc->load(options::run_file_path("cache"));

	/* Load the playlist from .moc directory. */
	str plist_file = options::run_file_path(PLAYLIST_FILE);
	if (is_plist_file(plist_file.c_str()))
	{
		plist pl;
		if (pl.load_m3u(plist_file))
			audio_plist_set_and_play(std::move(pl), -1);
	}

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
	Lock lock(*cli);
	return cli->socket->send((int)EV_DATA) && cli->socket->send(data);
}
static bool send_data_str (client *cli, const char *str)
{
	Lock lock(*cli);
	return cli->socket->send((int)EV_DATA) && cli->socket->send(str);
}

/* Add event to the client's queue */
void add_event (client &cli, int type)
{
	Lock lock(cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.finish();
}
template<typename T> void add_event (client &cli, int type, const T& data)
{
	Lock lock(cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.send(data);
	sock.finish();
}
template<typename T1, typename T2> void add_event (client &cli, int type, const T1 &d1, T2 d2)
{
	Lock lock(cli);
	auto &sock = *cli.socket;
	sock.packet(type);
	sock.send(d1);
	sock.send(d2);
	sock.finish();
}

template<typename T1, typename T2> void add_event_all(int type, const T1 &d1, T2 d2)
{
	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		add_event(clients[i], type, d1, d2);
		added = true;
	}

	if (added) wake_up_server ();
}
template<typename T1> void add_event_all(int type, const T1& d1)
{
	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		add_event(clients[i], type, d1);
		added = true;
	}

	if (added) wake_up_server ();
}
void add_event_all(int type)
{
	bool added = false;

	for (int i = 0; i < CLIENTS_MAX; i++)
	{
		if (!clients[i].socket) continue;
		add_event(clients[i], type);
		added = true;
	}

	if (added) wake_up_server ();
}

/* Send events from the queue. Return 0 on error. */
static bool flush_events (client &cli)
{
	if (!cli.socket) return false;
	Lock lock(cli);
	auto &sock = *cli.socket;
	while (!sock.packets.empty() && sock.send_next_packet_noblock() == 1) ;
	return true;
}

/* Send events to clients whose sockets are ready to write. */
static void send_events (fd_set *fds)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket && FD_ISSET(clients[i].socket->fd(), fds)) {
			debug ("Flushing events for client %d", i);
			if (!flush_events (clients[i])) del_client (clients[i]);
		}
}

/* End playing and cleanup. */
static void server_shutdown ()
{
	logit ("Server exiting...");

	str plist_file = options::run_file_path(PLAYLIST_FILE);
	plist playlist; audio_get_plist(playlist);
	if (playlist.size()) playlist.save(plist_file); else unlink (plist_file.c_str());

	audio_exit ();
	delete tc; tc = NULL;
	unlink (options::SocketPath.c_str());
	unlink (options::run_file_path(PID_FILE).c_str());
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
		int idx; audio_get_current(file, idx);
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

		str path; int idx; audio_get_current(path, idx);
		if (path.empty())
			return false;
	
		auto T = tc->get_immediate(path.c_str()).time;

		if (T <= 0) return false;
		sec = (T * sec)/100;
	}

	logit ("Jumping to %ds", sec);
	audio_jump_to (sec);
	return true;
}

void update_eq_name()
{
	char *n = equalizer_current_eqname();
	str msg = format("EQ set to %s", n);
	free(n);
	status_msg(msg.c_str());
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
}

void req_equalizer_prev() { equalizer_prev(); update_eq_name(); }
void req_equalizer_next() { equalizer_next(); update_eq_name(); }

void req_toggle_make_mono()
{
	softmixer_set_mono(!softmixer_is_mono());
	status_msg(format("Mono-Mixing set to: %s", softmixer_is_mono()?"on":"off").c_str());
}

static void send_ev_options(int where = -1)
{
	int v = options::AutoNext + 2*options::Repeat + 4*options::Shuffle;
	if (where == -1)
		add_event_all(EV_OPTIONS, v);
	else
		add_event(clients[where], EV_OPTIONS, v);
}
static void send_ev_mixer(int where = -1)
{
	char *name = audio_get_mixer_channel_name ();
	int v = audio_get_mixer();

	if (where == -1)
		add_event_all(EV_MIXER_CHANGE, name, v);
	else
		add_event(clients[where], EV_MIXER_CHANGE, name, v);

	free (name);
}

/* Receive a command from the client and execute it. */
static void handle_command (const int client_id)
{
	int cmd;
	int err = 0;
	client &cli = clients[client_id];

	if (!cli.socket->get(cmd)) {
		logit ("Failed to get command from the client");
		del_client (cli);
		return;
	}

	switch (cmd) {
		case CMD_QUIT:
			logit ("Exit request from the client");
			del_client (cli);
			server_quit = 1;
			break;
		case CMD_PLIST_ADD:
		{
			plist pl; int idx;
			if ((err = !cli.socket->get(pl))) break;
			if ((err = !cli.socket->get(idx))) break;
			logit ("Adding %d files to the list", (int)pl.size());
			audio_plist_add (pl, idx);
			debug ("Sending EV_PLIST_ADD");
			add_event_all(EV_PLIST_ADD, pl, idx);
			break;
		}
		case CMD_PLAY:
		{
			int idx;
			str file;
			if ((err = !cli.socket->get(idx))) break;
			if ((err = !cli.socket->get(file))) break;
			if (idx == -1)
			{
				logit ("Playing %s", file.empty() ? "first element on the list" : file.c_str());
				audio_play (file.c_str());
			}
			else if (file.empty())
			{
				audio_play (idx);
			}
			else
			{
				plist pl, tmp; pl += std::move(file);
				if ((err = !cli.socket->get(tmp))) break;
				pl += std::move(tmp);
				if (idx >= pl.size())
				{
					logit("Invalid play index %d for given playlist of size %d", idx, (int)pl.size());
					err = 1;
					break;
				}
				audio_plist_set_and_play(std::move(pl), idx);
				add_event_all(EV_PLIST_NEW);
			}
			break;
		}
		case CMD_PLIST_DEL:
		{
			int i, n;
			if ((err = !cli.socket->get(i))) break;
			if ((err = !cli.socket->get(n))) break;
			debug ("Request for deleting %d..%d", i, i+n-1);
			audio_plist_delete (i, n);
			debug ("Sending EV_PLIST_DEL");
			add_event_all (EV_PLIST_DEL, i, n);
			break;
		}
		case CMD_PLIST_GET:
		{
			Lock lock(cli);
			err = !cli.socket->send(EV_DATA) || 
				!audio_send_plist(*cli.socket);
			break;
		}
		case CMD_PLIST_MOVE:
		{
			int i, j;
			if ((err = !cli.socket->get(i) || !cli.socket->get(j))) break;
			audio_plist_move (i, j);
			debug ("Sending EV_PLIST_MOVE");
			add_event_all (EV_PLIST_MOVE, i, j);
			break;
		}
		case CMD_DISCONNECT:
			logit ("Client disconnected");
			del_client (cli);
			break;
		case CMD_PAUSE: audio_pause(); break;
		case CMD_UNPAUSE: audio_unpause(); break;
		case CMD_STOP: audio_stop(); break;
		case CMD_GET_CTIME:
			err = !send_data_int(&cli, MAX(0, audio_get_time()));
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
		case CMD_GET_CURRENT:
		{
			Lock lock(cli);
			str path; int idx; audio_get_current(path, idx);
			err = !cli.socket->send(EV_DATA) || 
				!cli.socket->send(idx) || !cli.socket->send(path);
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
		case CMD_GET_OPTIONS:
			send_ev_options(client_id);
			break;
		case CMD_SET_OPTION_AUTONEXT:
		{
			bool val = 0;
			if ((err = !cli.socket->get(val))) break;
			options::AutoNext = val;
			send_ev_options();
			break;
		}
		case CMD_SET_OPTION_SHUFFLE:
		{
			bool val = 0;
			if ((err = !cli.socket->get(val))) break;
			options::Shuffle = val;
			send_ev_options();
			break;
		}
		case CMD_SET_OPTION_REPEAT:
		{
			bool val = 0;
			if ((err = !cli.socket->get(val))) break;
			options::Repeat = val;
			send_ev_options();
			break;
		}
		case CMD_GET_MIXER:
			err = !send_data_int(&cli, audio_get_mixer());
			break;
		case CMD_SET_MIXER:
		{
			int val;
			err = !cli.socket->get(val);
			if (!err) audio_set_mixer (val);
			break;
		}
		case CMD_TOGGLE_MIXER_CHANNEL:
			audio_toggle_mixer_channel ();
			send_ev_mixer();
			break;
		case CMD_TOGGLE_SOFTMIXER:
			softmixer_set_active(!softmixer_is_active());
			send_ev_mixer();
			break;
		case CMD_GET_MIXER_CHANNEL_NAME:
		{
			char *name = audio_get_mixer_channel_name ();
			err = !send_data_str(&cli, name);
			free (name);
			break;
		}
		case CMD_GET_FILE_TAGS:
		{
			str file;
			if (!clients[client_id].socket->get(file)) { err = 1; break; }
			tc->add_request (file.c_str(), client_id);
			break;
		}
		case CMD_SET_FILE_TAGS:
		{
			str file;
			if (!clients[client_id].socket->get(file)) { err = 1; break; }
			auto *tags = clients[client_id].socket->get_tags();
			if (!tags) { err = 1; break; }
			tc->add_request (file.c_str(), client_id, tags);
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
		del_client (cli);
	}
}

/* Add clients file descriptors to fds. */
static void add_clients_fds (fd_set *read, fd_set *write)
{
	for (int i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket) {
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
	for (int i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket && FD_ISSET(clients[i].socket->fd(), fds))
			handle_command (i);
}

/* Close all client connections sending EV_EXIT. */
static void close_clients ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket) {
			clients[i].socket->send(EV_EXIT);
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
	add_event_all (EV_BITRATE, sound_info.bitrate);
}

void set_info_channels (const int channels)
{
	sound_info.channels = channels;
	add_event_all (EV_CHANNELS, sound_info.channels);
}

void set_info_rate (const int rate)
{
	sound_info.rate = rate;
	add_event_all (EV_RATE, sound_info.rate);
}

void set_info_avg_bitrate (const int avg_bitrate)
{
	sound_info.avg_bitrate = avg_bitrate;
	add_event_all (EV_AVG_BITRATE, sound_info.avg_bitrate);
}

/* Notify the client about change of the player state. */
void state_change ()
{
	add_event_all (EV_STATE);
}

void ctime_change ()
{
	add_event_all (EV_CTIME, (int)MAX(0, audio_get_time()));
}

void status_msg (const str &msg)
{
	logit("%s", msg.c_str());
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
}

void ev_audio_stop ()
{
}

void ev_audio_fail (const str &path)
{
	audio_fail_file(path);
}
