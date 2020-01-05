#include <stdarg.h>
#include <locale.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/select.h>
#include <wctype.h>
#include <wchar.h>

#include "client.h"
#include "../lists.h"
#include "../files.h"
#include "../input/decoder.h"
#include "../playlist.h"
#include "../protocol.h"
#include "utf8.h"
#include "../rcc.h"
#include "../output/softmixer.h"
#include "../server/ratings.h"

#define QUEUE_CLEAR_THRESH 128
#define STARTUP_MESSAGE "Welcome to " PACKAGE_NAME " (version " PACKAGE_VERSION ")!"
#define HISTORY_SIZE	50

volatile int  Client::want_quit = 0; // 1=quit client, 2=quit server
volatile bool Client::want_interrupt = false;
volatile bool Client::want_resize = false;

static void sig_quit (int sig)
{
	log_signal (sig);
	Client::want_interrupt = true;
	Client::want_quit = 1;
}
static void sig_interrupt (int sig)
{
	log_signal (sig);
	Client::want_interrupt = true;
}
static void sig_winch (int sig)
{
	log_signal (sig);
	Client::want_resize = true;
}

/* Wait for EV_DATA handling other events. */
void Client::wait_for_data ()
{
	logit("waiting for data");
	while (true) {
		int event = srv.get_int();
		if (event == EV_DATA) break;
		handle_server_event(event);
		if (event == EV_EXIT) return;
	}
	logit("found EV_DATA");
}

void Client::send_tags_request (const str &file)
{
	srv.send(CMD_GET_FILE_TAGS);
	srv.send(file);
	debug ("Asking for tags for %s", file.c_str());
}

/* Get the server options and set our options like them. */
void Client::get_server_options ()
{
	srv.send(CMD_GET_OPTION_SHUFFLE);
	options::Shuffle = get_data_bool ();

	srv.send(CMD_GET_OPTION_REPEAT);
	options::Repeat = get_data_bool ();

	srv.send(CMD_GET_OPTION_AUTONEXT);
	options::AutoNext = get_data_bool ();

	iface->redraw(1);
}

int Client::get_mixer_value() { srv.send(CMD_GET_MIXER); return get_data_int (); }
int Client::get_channels() { srv.send(CMD_GET_CHANNELS); return get_data_int (); }
int Client::get_rate() { srv.send(CMD_GET_RATE); return get_data_int (); }
int Client::get_bitrate() { srv.send(CMD_GET_BITRATE); return get_data_int (); }
int Client::get_avg_bitrate() { srv.send(CMD_GET_AVG_BITRATE); return get_data_int (); }
int Client::get_curr_time() { srv.send(CMD_GET_CTIME); return get_data_int (); }
str Client::get_curr_file() { srv.send(CMD_GET_SNAME); return get_data_str (); }
PlayState Client::get_state() { srv.send(CMD_GET_STATE); return (PlayState)get_data_int (); }
void Client::update_mixer_name ()
{
	srv.send(CMD_GET_MIXER_CHANNEL_NAME);
	str name = get_data_str();
	debug ("Mixer name: %s", name.c_str());
	iface->update_mixer_name(name);
	iface->update_mixer_value(get_mixer_value());
}

/* Make new cwd path from CWD and this path. */
void Client::set_cwd(const str &path)
{
	if (path[0] == '/')
		cwd = path;
	else
	{
		if (cwd.empty()) {
			char buf[PATH_MAX];
			if (!getcwd(buf, sizeof(buf))) fatal ("Can't get CWD: %s", xstrerror (errno));
			cwd = buf;
		}
		if (cwd.back() != '/') cwd += '/';
		cwd += path;
	}
	cwd = resolve_path(cwd);
}

/* Try to find the directory we can start and set cwd to it. */
void Client::set_start_dir ()
{
	char buf[PATH_MAX];
	if (getcwd(buf, sizeof (buf)))
		cwd = buf;
	else {
		if (errno == ERANGE) fatal ("CWD is larger than PATH_MAX!");
		cwd = get_home();
	}
}

/* Set cwd to last directory written to a file, return 1 on success. */
bool Client::read_last_dir ()
{
	FILE *dir_file = fopen(create_file_name("last_directory"), "r");
	if (!dir_file) return 0;
	
	char buf[PATH_MAX];
	int n = fread(buf, 1, sizeof(buf)-1, dir_file);
	fclose (dir_file);
	if (!n) return false;
	buf[n] = 0;
	cwd = buf;
	return true;
}

/* For each file in the playlist, send a request for all the given tags if
 * the file is missing any of those tags.  Return the number of requests. */
void Client::ask_for_tags (const plist &plist)
{
	for (auto &i : plist.items)
	{
		if (i->tags || i->type != F_SOUND) continue;
		send_tags_request(i->path);
	}
}

void Client::interface_message (const char *format, ...)
{
	va_list va; va_start (va, format);
	char *msg = format_msg_va (format, va);
	va_end (va);
	iface->message (msg);
	free (msg);
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
void Client::update_item_tags (plist &plist, const int num, file_tags *tags)
{
	if (num < 0 || num >= plist.size()) return;
	plist_item &i = *plist.items[num];
	if (i.tags) *i.tags = *tags; else i.tags.reset(new file_tags(*tags));
}

/* Use new tags for current file title (for Internet streams). */
void Client::update_curr_tags ()
{
	auto f = iface->get_curr_file();
	if (is_url(f.c_str())) {
		srv.send(CMD_GET_TAGS);
		wait_for_data ();
		iface->update_curr_tags(srv.get_tags());
	}
}

/* Get and show the server state. */
void Client::update_state ()
{
	auto old_state = iface->get_state();
	auto new_state = get_state ();
	iface->update_state(new_state);

	/* Silent seeking makes no sense if the state has changed. */
	if (old_state != new_state) silent_seek_pos = -1;

	str file = get_curr_file();
	if (iface->get_curr_file() != file)
	{
		iface->update_curr_file(file);
		iface->update_curr_tags(nullptr);
		silent_seek_pos = -1;
	}

	iface->update_channels(get_channels());
	iface->update_bitrate(get_bitrate());
	iface->update_rate(get_rate());
	if (silent_seek_pos == -1) iface->update_curr_time(get_curr_time ());
}

/* Handle server event. */
void Client::handle_server_event (int type)
{
	logit ("EVENT: 0x%02x", type);
	switch (type)
	{
		case EV_EXIT: interface_fatal ("The server exited!"); break;
		case EV_BUSY: interface_fatal ("The server is busy; too many other clients are connected!"); break;

		case EV_CTIME:
			if (silent_seek_pos == -1) iface->update_curr_time(get_curr_time ());
			break;
		case EV_STATE: update_state(); break;
		case EV_BITRATE: iface->update_bitrate(get_bitrate()); break;
		case EV_RATE:  iface->update_rate(get_rate()); break;
		case EV_CHANNELS: iface->update_channels(get_channels()); break;
		case EV_AVG_BITRATE: iface->update_avg_bitrate(get_avg_bitrate()); break;
		case EV_OPTIONS: get_server_options(); break;
		case EV_SRV_ERROR: iface->error_message(srv.get_str()); break;
		case EV_PLIST_NEW:
			if (!synced) break;
			srv.send(CMD_PLIST_GET);
			wait_for_data();
			srv.get(playlist);
			ask_for_tags(playlist);
			break;
		case EV_PLIST_ADD:
		{
			if (!synced) break;
			plist pl; srv.get(pl);
			playlist += pl;
			ask_for_tags(pl);
			iface->redraw(2);
			break;
		}
		case EV_PLIST_DEL:
		{
			if (!synced) break;
			int i = srv.get_int();
			playlist.remove(i);
			iface->redraw(2);
			break;
		}
		case EV_PLIST_MOVE:
		{
			if (!synced) break;
			int i = srv.get_int(), j = srv.get_int();
			playlist.move(i, j);
			iface->redraw(2);
			break;
		}
		case EV_TAGS:
			update_curr_tags ();
			break;
		case EV_STATUS_MSG: iface->status(srv.get_str()); break;
		case EV_MIXER_CHANGE: update_mixer_name(); break;
		case EV_FILE_TAGS:
		{
			str file = srv.get_str();
			file_tags *tags = srv.get_tags();
			logit ("Received tags for %s", file.c_str());
			iface->redraw(2);
			int n;
			if ((n = dir_plist.find(file)) != -1) update_item_tags (dir_plist, n, tags);
			if ((n = playlist.find(file))  != -1) update_item_tags (playlist, n, tags);
			if (iface->get_curr_file() == file) {
				debug ("Tags apply to the currently played file.");
				if (tags->time != -1) iface->update_total_time(tags->time);
				iface->update_curr_tags(tags);
			}
			else delete tags;
			break;
		}
		case EV_FILE_RATING:
		{
			str file = srv.get_str();
			int rating = srv.get_int();
			debug ("Received rating for %s", file.c_str());
			iface->redraw(2);
			int n;
			if ((n = dir_plist.find(file)) != -1) {
				plist_item &i = *dir_plist.items[n];
				if (i.tags) i.tags->rating = rating;
			}
			if ((n = playlist.find(file)) != -1) {
				plist_item &i = *playlist.items[n];
				if (i.tags) i.tags->rating = rating;
			}
			break;
		}
		case EV_AUDIO_START: break;
		case EV_AUDIO_STOP:  break;
		default:
			interface_fatal ("Unknown event: 0x%02x!", type);
			break;
	}
}

/* Load the directory content into dir_plist and switch the menu to it.
 * If dir is NULL, go to the cwd.  If reload is not zero, we are reloading
 * the current directory, so use iface_update_dir_content().
 * Return 1 on success, 0 on error. */
bool Client::go_to_dir (const char *dir)
{
	str last_dir = cwd; // for selecting the old dir after going up

	iface->status("Reading directory...");
	plist bak; bak.swap(dir_plist);
	if (!dir_plist.load_directory(dir ? dir : cwd.c_str())) {
		iface->status ("Failed.");
		bak.swap(dir_plist);
		return false;
	}

	/* TODO: use CMD_ABORT_TAGS_REQUESTS (what if we requested tags for the playlist?) */

	if (dir) cwd = dir;
	if (options::ReadTags) ask_for_tags (dir_plist);
	iface->redraw(2);
	iface->select_path(last_dir);

	iface->status ("");
	return true;
}

/* Load the playlist file and switch the menu to it. Return 1 on success. */
int Client::go_to_playlist (const char *file)
{
	if (playlist.size()) {
		error ("Please clear the playlist, because "
				"I'm not sure you want to do this.");
		return 0;
	}

	synced = false;

	iface->status("Loading playlist...");
	if (playlist.load_m3u(file)) {
		iface->message ("Playlist loaded.");
	}
	else {
		iface->message ("The playlist is empty");
		iface->status ("");
		return 0;
	}

	return 1;
}

/* Enter to the initial directory or toggle to the initial playlist (only
 * if the function has not been called yet). */
void Client::enter_first_dir ()
{
	static int first_run = 1;

	if (options::StartInMusicDir) {
		const char *p = options::MusicDir.c_str();
		if (p && *p) {
			set_cwd (p);
			if (first_run && plist_item::ftype(p) == F_PLAYLIST
					&& playlist.size() == 0
					&& go_to_playlist(p)) {
				cwd = "";
				first_run = 0;
			}
			else if (plist_item::ftype(cwd) == F_DIR && go_to_dir(NULL)) {
				first_run = 0;
				return;
			}
		}
		else
			error ("MusicDir is not set");
	}

	if (!(read_last_dir() && go_to_dir(NULL))) {
		set_start_dir ();
		if (!go_to_dir(NULL))
			interface_fatal ("Can't enter any directory!");
	}

	first_run = 0;
}

/* Process file names passed as arguments. */
void Client::process_args (stringlist &args)
{
	int size;
	const char *arg;

	size = (int)args.size();
	arg = args[0].c_str();

	if (size == 1 && is_dir(arg)) {
		set_cwd (arg);
		if (!go_to_dir (NULL)) enter_first_dir ();
		return;
	}

	if (size == 1 && is_plist_file (arg))
	{
		char path[PATH_MAX + 1];   /* the playlist's directory */
		if (arg[0] == '/')
			strcpy (path, "/");
		else if (!getcwd (path, sizeof (path)))
			interface_fatal ("Can't get CWD: %s", xstrerror (errno));

		resolve_path (path, sizeof (path), arg);
		char *slash = strrchr (path, '/');
		assert (slash != NULL);
		*slash = 0;

		iface->status("Loading playlist...");
		playlist.load_m3u(path);
		iface->status("");
		enter_first_dir ();
		return;
	}

	char this_cwd[PATH_MAX];
	if (!getcwd (this_cwd, sizeof (cwd)))
		interface_fatal ("Can't get CWD: %s", xstrerror (errno));

	for (int ix = 0; ix < size; ix += 1) {
		char path[2 * PATH_MAX];

		const char *arg = args[ix].c_str();
		bool dir = is_dir (arg);

		if (is_url (arg)) {
			strncpy (path, arg, sizeof (path));
			path[sizeof(path) - 1] = 0;
		}
		else {
			if (arg[0] == '/')
				strcpy (path, "/");
			else
				strcpy (path, this_cwd);
			resolve_path (path, sizeof (path), arg);
		}

		if (dir == 1)
			playlist.add_directory(path, true);
		else if (!dir && (is_sound_file (path) || is_url (path))) {
			playlist += path;
		}
		else if (is_plist_file (path)) {
			playlist.load_m3u(path);
		}
	}

	enter_first_dir ();
}

void Client::go_dir_up ()
{
	if (cwd.empty() || cwd == "/") return;
	auto i = cwd.rfind('/', cwd.length()-1);
	if (i == 0 || i == str::npos)
		cwd = "/";
	else
		cwd = cwd.substr(0, i);
	go_to_dir (cwd.c_str());
}

void Client::play_it(const str &file)
{
	if (iface->in_dir_plist())
	{
		auto *i = iface->sel_item();
		if (!i) return;
		srv.send(CMD_PLAY);
		srv.send(-1);
		srv.send(file);
	}
	else
	{
		int i = iface->sel_index();
		if (i < 0) return;
		srv.send(CMD_PLAY);
		srv.send(iface->sel_index());
		if (!synced)
		{
			srv.send(playlist);
			synced = true;
		}
		else srv.send("");
	}
}

/* Action when the user selected a file. */
void Client::go_file ()
{
	const plist_item *i = iface->sel_item();
	if (!i) return;
	if (i->type == F_SOUND || i->type == F_URL)
		play_it (i->path);
	else if (i->type == F_DIR && iface->in_dir_plist()) {
		go_to_dir (i->path.c_str());
	}
	else if (i->type == F_PLAYLIST)
		go_to_playlist (i->path.c_str());
}

void Client::set_mixer (int val)
{
	val = CLAMP(0, val, 100);
	srv.send(CMD_SET_MIXER);
	srv.send(val);
}

void Client::adjust_mixer (const int diff)
{
	set_mixer (get_mixer_value() + diff);
}

/* Recursively add the content of a directory to the playlist. */
void Client::add_dir_plist ()
{
	if (!iface->in_dir_plist()) {
		error ("Can't add to the playlist a file from the playlist.");
		return;
	}

	auto *item = iface->sel_item ();
	if (!item) return;

	auto type = item->type;
	if (type != F_DIR && type != F_PLAYLIST) {
		error ("This is not a directory or a playlist.");
		return;
	}

	iface->status("Reading directories...");

	plist pl;
	if (type == F_DIR)
		pl.add_directory(item->path.c_str());
	else
		pl.load_m3u(item->path.c_str());

	if (synced)
	{
		srv.send(CMD_PLIST_ADD);
		srv.send(pl);
	}
	else
	{
		playlist += std::move(pl);
		iface->redraw(2);
	}

	iface->status("");
}

/* Add the currently selected file to the playlist. */
void Client::add_file_plist ()
{
	if (!iface->in_dir_plist()) {
		error ("Can't add to the playlist a file from the playlist.");
		return;
	}

	auto *item = iface->sel_item();
	if (!item) return;
	
	if (item->type == F_DIR) {
		add_dir_plist();
		return;
	}

	if (item->type != F_SOUND) {
		error ("You can only add a file using this command.");
		return;
	}

	if (synced) {
		srv.send(CMD_PLIST_ADD);
		srv.send(item->path);
		srv.send("");
	}
	else
	{
		playlist += *item;
		iface->redraw(2);
	}
	iface->move_down();
}

void Client::set_rating (int r)
{
	assert (r >= 0 && r <= 5);

	auto *item = iface->sel_item();
	if (!item || item->type != F_SOUND) return;

	srv.send(CMD_SET_RATING);
	srv.send(item->path);
	srv.send(r);
}

/* Switch ReadTags options and update the menu. */
void Client::switch_read_tags ()
{
	options::ReadTags ^= 1;
	iface->status(options::ReadTags ? "ReadTags: yes" : "ReadTags: no");
	if (options::ReadTags) {
		ask_for_tags(dir_plist);
		ask_for_tags(playlist);
	}
	iface->redraw(2);
}

void Client::delete_item ()
{
	if (iface->in_dir_plist()) {
		error ("You can only delete an item from the playlist.");
		return;
	}

	assert (playlist.size() > 0);
	int i = iface->sel_index();
	if (i < 0) return;

	if (synced)
	{
		srv.send(CMD_PLIST_DEL);
		srv.send(i);
	}
	else
	{
		playlist.remove(i);
		iface->redraw(2);
	}
}

/* Select the file that is currently played. */
void Client::go_to_playing_file ()
{
	auto *item = iface->sel_item();
	if (!item || item->type != F_SOUND) return;

	str path = item->path;
	auto i = path.rfind('/');
	if (i == 0 || i == str::npos) path = "/"; else path = path.substr(0, i);
	go_to_dir(path.c_str());

	iface->sel_item(item, 0);
}

/* Return the time like the standard time() function, but rounded i.e. if we
 * have 11.8 seconds, return 12 seconds. */
static time_t rounded_time ()
{
	struct timespec exact_time;
	time_t curr_time;

	if (get_realtime (&exact_time) == -1)
		interface_fatal ("get_realtime() failed: %s", xstrerror (errno));

	curr_time = exact_time.tv_sec;
	if (exact_time.tv_nsec > 500000000L)
		curr_time += 1;

	return curr_time;
}

/* Handle silent seek key. */
void Client::seek_silent (int sec)
{
	if (iface->get_state() != STATE_PLAY || is_url(iface->get_curr_file().c_str())) return;

	if (silent_seek_pos == -1)
		silent_seek_pos = iface->get_curr_time() + sec;
	else
		silent_seek_pos += sec;

	silent_seek_pos = CLAMP(0, silent_seek_pos, iface->get_total_time());

	silent_seek_key_last = rounded_time ();
	iface->update_curr_time(silent_seek_pos);
}

/* Move the current playlist item (direction: 1 - up, -1 - down). */
void Client::move_item (int direction)
{
	if (iface->in_dir_plist()) {
		error ("You can move only playlist items.");
		return;
	}

	assert (direction == -1 || direction == 1);
	int i = iface->sel_index(), j = i+direction;
	if (i < 0 || j < 0 || j >= playlist.size()) return;

	if (synced)
	{
		srv.send(CMD_PLIST_MOVE);
		srv.send(i);
		srv.send(j);
	}
	else
	{
		playlist.move(i, j);
		iface->redraw(2);
	}
}

Client::Client(int sock, stringlist &args)
: srv(sock, false), synced(false)
{
	logit ("Starting MOC Interface");

	/* Set locale according to the environment variables. */
	if (!setlocale(LC_CTYPE, "")) logit ("Could not set locale!");

	keys_init ();
	iface.reset(new Interface(*this, dir_plist, playlist));
	get_server_options ();
	update_mixer_name ();

	xsignal(SIGQUIT, sig_quit);
	xsignal(SIGTERM, sig_quit);
	xsignal(SIGHUP, sig_quit);
	xsignal(SIGINT, sig_interrupt);
	xsignal(SIGWINCH, sig_winch);

	if (!args.empty()) {
		process_args (args);

		if (playlist.size() == 0) {
			srv.send(CMD_PLIST_GET);
			wait_for_data();
			srv.get(playlist);
			ask_for_tags(playlist);
			synced = true;
		} else {
			/* Now enter_first_dir() should not go to the music
			 * directory. */
			options::StartInMusicDir = false;
		}
	}
	else {
		srv.send(CMD_PLIST_GET);
		wait_for_data();
		srv.get(playlist);
		ask_for_tags(playlist);
		synced = true;
		enter_first_dir ();
	}

	update_state ();
}

void Client::run()
{
	while (!want_quit) {
		fd_set fds;
		int ret;
		struct timespec timeout = { 1, 0 };
		int srv_sock = srv.fd();
		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		ret = pselect (srv_sock + 1, &fds, NULL, NULL, &timeout, NULL);
		if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("pselect() failed: %s", xstrerror (errno));

		if (want_resize)
		{
			iface->resize();
			logit ("resize");
			want_resize = 0;
		}

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds)) {
				iface->handle_input();
				Client::want_interrupt = false;
			}

			if (!want_quit && FD_ISSET(srv_sock, &fds))
			{
				int type;
				if (srv.get_int_noblock(type))
					handle_server_event(type);
				else
					debug ("Getting event would block.");
			}
		}

		if (!want_quit)
		{
			iface->update_mixer_value(get_mixer_value());

			time_t curr_time = time(NULL);
			if (silent_seek_pos != -1 && silent_seek_key_last < curr_time)
			{
				seek (silent_seek_pos - iface->get_curr_time() - 1);
				silent_seek_pos = -1;
			}
		}

		iface->draw();
	}
}

Client::~Client ()
{
	FILE *dir_file = fopen(create_file_name("last_directory"), "w");
	if (!dir_file) {
		error_errno ("Can't save current directory", errno);
	} else {
		fprintf (dir_file, "%s", cwd.c_str());
		fclose (dir_file);
	}

	srv.send(want_quit > 1 ? CMD_QUIT : CMD_DISCONNECT);

	iface = nullptr; // deletes the interface
	logit ("Interface exited");
}

void interface_fatal (const char *format, ...)
{
	char *msg;
	va_list va;

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);

	fatal ("%s", msg);
}

void Client::handle_command(key_cmd cmd)
{
	switch (cmd)
	{
		case KEY_CMD_QUIT_CLIENT: want_quit = 1; return;
		case KEY_CMD_QUIT:        want_quit = 2; return;

		case KEY_CMD_GO: go_file(); break;
		case KEY_CMD_STOP: srv.send(CMD_STOP); break;
		case KEY_CMD_NEXT:
		{
			if (iface->get_state() != STATE_STOP)
				srv.send(CMD_NEXT);
			else if (playlist.size()) {
				srv.send(CMD_PLAY);
				srv.send("");
			}
			break;
		}
		case KEY_CMD_PREVIOUS: srv.send(CMD_PREV); break;
		case KEY_CMD_PAUSE:
			switch (iface->get_state()) {
				case STATE_PLAY: srv.send(CMD_PAUSE); break;
				case STATE_PAUSE: srv.send(CMD_UNPAUSE); break;
				default: logit ("User pressed pause when not playing."); break;
			}
			break;
		case KEY_CMD_TOGGLE_READ_TAGS:
			switch_read_tags ();
			break;
		case KEY_CMD_TOGGLE_SHUFFLE:
			srv.send(CMD_SET_OPTION_SHUFFLE);
			srv.send(!options::Shuffle);
			get_server_options();
			break;
		case KEY_CMD_TOGGLE_REPEAT:
			srv.send(CMD_SET_OPTION_REPEAT);
			srv.send(!options::Repeat);
			get_server_options();
			break;
		case KEY_CMD_TOGGLE_AUTO_NEXT:
			srv.send(CMD_SET_OPTION_AUTONEXT);
			srv.send(!options::AutoNext);
			get_server_options();
			break;
		case KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS:
			options::PlaylistFullPaths ^= 1;
			iface->redraw(2);
			break;
		case KEY_CMD_PLIST_ADD_FILE:
			add_file_plist ();
			break;
		case KEY_CMD_PLIST_CLEAR:
			if (synced)
			{
				playlist.clear();
				synced = false;
			}
			else
			{
				srv.send(CMD_PLIST_GET);
				wait_for_data();
				srv.get(playlist);
				ask_for_tags(playlist);
				synced = true;
			}
			iface->redraw(2);
			break;
		case KEY_CMD_PLIST_ADD_DIR:
			add_dir_plist ();
			break;
		case KEY_CMD_MIXER_DEC_1: adjust_mixer (-1); break;
		case KEY_CMD_MIXER_DEC_5: adjust_mixer (-5); break;
		case KEY_CMD_MIXER_INC_5: adjust_mixer (+5); break;
		case KEY_CMD_MIXER_INC_1: adjust_mixer (+1); break;
		case KEY_CMD_SEEK_BACKWARD: seek (-options::SeekTime); break;
		case KEY_CMD_SEEK_FORWARD: seek (options::SeekTime); break;

		case KEY_CMD_SEEK_0: seek_to_percent (0 * 10); break;
		case KEY_CMD_SEEK_1: seek_to_percent (1 * 10); break;
		case KEY_CMD_SEEK_2: seek_to_percent (2 * 10); break;
		case KEY_CMD_SEEK_3: seek_to_percent (3 * 10); break;
		case KEY_CMD_SEEK_4: seek_to_percent (4 * 10); break;
		case KEY_CMD_SEEK_5: seek_to_percent (5 * 10); break;
		case KEY_CMD_SEEK_6: seek_to_percent (6 * 10); break;
		case KEY_CMD_SEEK_7: seek_to_percent (7 * 10); break;
		case KEY_CMD_SEEK_8: seek_to_percent (8 * 10); break;
		case KEY_CMD_SEEK_9: seek_to_percent (9 * 10); break;

		case KEY_CMD_GO_TO_PLAYING_FILE:
			go_to_playing_file ();
			break;
		/*case KEY_CMD_HIDE_MESSAGE:
			iface_disable_message ();
			break;*/
		case KEY_CMD_RELOAD:
			go_to_dir(NULL);
			break;
		case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
			options::ShowHiddenFiles ^= 1;
			go_to_dir(NULL);
			break;
		case KEY_CMD_GO_MUSIC_DIR:
		{
			char music_dir[PATH_MAX] = "/";

			if (options::MusicDir.empty()) {
				error ("MusicDir not defined");
				return;
			}

			resolve_path (music_dir, sizeof(music_dir), options::MusicDir.c_str());

			switch (plist_item::ftype(music_dir)) {
				case F_DIR: go_to_dir (music_dir); break;
				case F_PLAYLIST: go_to_playlist (music_dir); break;
				default: error ("MusicDir is neither a directory nor a playlist!"); break;
			}
			break;
		}
		case KEY_CMD_PLIST_DEL:
			delete_item ();
			break;
		case KEY_CMD_GO_DIR_UP:
			go_dir_up ();
			break;
		case KEY_CMD_WRONG:
			error ("Bad command");
			break;
		case KEY_CMD_SEEK_FORWARD_5: seek_silent (options::SilentSeekTime); break;
		case KEY_CMD_SEEK_BACKWARD_5: seek_silent (-options::SilentSeekTime); break;
		case KEY_CMD_VOLUME_10: set_mixer (10); break;
		case KEY_CMD_VOLUME_20: set_mixer (20); break;
		case KEY_CMD_VOLUME_30: set_mixer (30); break;
		case KEY_CMD_VOLUME_40: set_mixer (40); break;
		case KEY_CMD_VOLUME_50: set_mixer (50); break;
		case KEY_CMD_VOLUME_60: set_mixer (60); break;
		case KEY_CMD_VOLUME_70: set_mixer (70); break;
		case KEY_CMD_VOLUME_80: set_mixer (80); break;
		case KEY_CMD_VOLUME_90: set_mixer (90); break;
		case KEY_CMD_RATE_0: set_rating (0); break;
		case KEY_CMD_RATE_1: set_rating (1); break;
		case KEY_CMD_RATE_2: set_rating (2); break;
		case KEY_CMD_RATE_3: set_rating (3); break;
		case KEY_CMD_RATE_4: set_rating (4); break;
		case KEY_CMD_RATE_5: set_rating (5); break;
		case KEY_CMD_TOGGLE_MIXER: srv.send(CMD_TOGGLE_MIXER_CHANNEL); break;
		case KEY_CMD_TOGGLE_SOFTMIXER: srv.send(CMD_TOGGLE_SOFTMIXER); break;
		case KEY_CMD_TOGGLE_EQUALIZER: srv.send(CMD_TOGGLE_EQUALIZER); break;
		case KEY_CMD_EQUALIZER_REFRESH: srv.send(CMD_EQUALIZER_REFRESH); break;
		case KEY_CMD_EQUALIZER_PREV: srv.send(CMD_EQUALIZER_PREV); break;
		case KEY_CMD_EQUALIZER_NEXT: srv.send(CMD_EQUALIZER_NEXT); break;
		case KEY_CMD_TOGGLE_MAKE_MONO: srv.send(CMD_TOGGLE_MAKE_MONO); break;
		case KEY_CMD_PLIST_MOVE_UP: move_item (1); break;
		case KEY_CMD_PLIST_MOVE_DOWN: move_item (-1); break;
		default:
			abort ();
	}
}
