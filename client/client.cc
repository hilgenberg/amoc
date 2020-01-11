#include "client.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>

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
	SOCKET_DEBUG("waiting for data");
	while (true) {
		int event = srv.get_int();
		if (event == EV_DATA) break;
		handle_server_event(event);
		if (event == EV_EXIT) return;
	}
	SOCKET_DEBUG("found EV_DATA");
}

void Client::send_tags_request (const str &file)
{
	srv.send(CMD_GET_FILE_TAGS);
	srv.send(file);
	debug ("Asking for tags for %s", file.c_str());
}

int Client::get_mixer_value() { srv.send(CMD_GET_MIXER); return get_data_int (); }
int Client::get_channels() { srv.send(CMD_GET_CHANNELS); return get_data_int (); }
int Client::get_rate() { srv.send(CMD_GET_RATE); return get_data_int (); }
int Client::get_bitrate() { srv.send(CMD_GET_BITRATE); return get_data_int (); }
int Client::get_avg_bitrate() { srv.send(CMD_GET_AVG_BITRATE); return get_data_int (); }
int Client::get_curr_time() { srv.send(CMD_GET_CTIME); return get_data_int (); }
PlayState Client::get_state() { srv.send(CMD_GET_STATE); return (PlayState)get_data_int (); }

/* Make new cwd path from CWD and this path. */
void Client::set_cwd(const str &path)
{
	cwd = absolute_path(add_path(cwd, path));
	normalize_path(cwd);
}

/* Set cwd to last directory written to a file, return 1 on success. */
bool Client::read_last_dir ()
{
	FILE *dir_file = fopen(options::config_file_path("last_directory").c_str(), "r");
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

/* Get and show the server state. */
void Client::update_state ()
{
	want_state_update = false;

	auto old_state = iface->get_state();
	auto new_state = get_state ();
	iface->update_state(new_state);

	/* Silent seeking makes no sense if the state has changed. */
	if (old_state != new_state) silent_seek_pos = -1;

	srv.send(CMD_GET_CURRENT);
	wait_for_data(); int idx = srv.get_int(); str file = srv.get_str();
	
	if (iface->update_curr_file(file, idx))
	{
		silent_seek_pos = -1;
		if (options::ReadTags && !file.empty())
			send_tags_request(file); // TODO: see if we already have them first
	}

	iface->update_channels(get_channels());
	iface->update_bitrate(get_bitrate());
	iface->update_rate(get_rate());
	if (silent_seek_pos == -1) iface->update_curr_time(get_curr_time ());
}

/* Handle server event. */
void Client::handle_server_event (int type)
{
	// this can not send any commands that return EV_DATA because it
	// gets called by wait_for_data!
	SOCKET_DEBUG("EVENT: %d", type);
	switch (type)
	{
		case EV_EXIT: interface_fatal ("The server exited!"); break;
		case EV_BUSY: interface_fatal ("The server is busy; too many other clients are connected!"); break;

		case EV_STATE: want_state_update = true; break;
		case EV_CTIME: { int tmp = srv.get_int(); if (silent_seek_pos == -1) iface->update_curr_time(tmp); } break;
		case EV_BITRATE: iface->update_bitrate(srv.get_int()); break;
		case EV_RATE:  iface->update_rate(srv.get_int()); break;
		case EV_CHANNELS: iface->update_channels(srv.get_int()); break;
		case EV_AVG_BITRATE: iface->update_avg_bitrate(srv.get_int()); break;
		case EV_OPTIONS: 
		{
			int v = srv.get_int();
			options::AutoNext = v & 1;
			options::Repeat   = v & 2;
			options::Shuffle  = v & 4;
			iface->redraw(1);
			break;
		}
		case EV_SRV_ERROR: iface->error_message(srv.get_str()); break;
		case EV_PLIST_NEW:
			if (synced) want_plist_update = true;
			break;
		case EV_PLIST_ADD:
		{
			plist pl; srv.get(pl);
			if (!synced || want_plist_update) break;
			playlist += pl;
			ask_for_tags(pl);
			iface->redraw(2);
			break;
		}
		case EV_PLIST_DEL:
		{
			int i = srv.get_int();
			int n = srv.get_int();
			if (!synced || want_plist_update) break;
			playlist.remove(i, n);
			iface->redraw(2);
			break;
		}
		case EV_PLIST_MOVE:
		{
			int i = srv.get_int(), j = srv.get_int();
			if (!synced || want_plist_update) break;
			playlist.move(i, j);
			iface->redraw(2);
			break;
		}

		case EV_STATUS_MSG: iface->message(srv.get_str()); break;
		case EV_MIXER_CHANGE:
			iface->update_mixer_name(srv.get_str());
			iface->update_mixer_value(srv.get_int());
			break;
		case EV_FILE_TAGS:
		{
			str file = srv.get_str();
			file_tags *tags = srv.get_tags();
			logit ("Received tags for %s", file.c_str());
			playlist.set_tags(file, tags);
			dir_plist.set_tags(file, tags);
			if (iface->get_curr_file() == file) {
				debug ("Tags apply to the currently played file.");
				iface->update_curr_tags(tags);
			}
			else delete tags;
			iface->redraw(2);
			break;
		}
		case EV_FILE_RATING:
		{
			str file = srv.get_str();
			int rating = srv.get_int();
			debug ("Received rating for %s", file.c_str());
			playlist.set_rating(file, rating);
			dir_plist.set_rating(file, rating);
			iface->redraw(2);
			break;
		}
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
	bool same = !dir || cwd==dir;
	auto &left = iface->left;
	int sel0 = left.sel, top0 = left.top;

	str last_dir = cwd; // for selecting the old dir after going up

	iface->status("Reading directory...");
	plist bak; bak.swap(dir_plist);
	if (!dir_plist.load_directory(dir ? str(dir) : cwd)) {
		iface->status ("Failed.");
		bak.swap(dir_plist);
		return false;
	}

	/* TODO: use CMD_ABORT_TAGS_REQUESTS (what if we requested tags for the playlist?) */

	if (dir) cwd = dir;
	if (options::ReadTags) ask_for_tags (dir_plist);
	if (same)
	{
		left.top = top0;
		left.sel = sel0;
	}
	else
	{
		left.top = 0;
		iface->select_path(last_dir);
	}

	iface->redraw(2);
	iface->status ("");
	return true;
}

/* Load the playlist file and switch the menu to it. Return 1 on success. */
int Client::go_to_playlist (const char *file)
{
	iface->status("Loading playlist...");
	if (playlist.load_m3u(file)) {
		iface->message ("Playlist loaded.");
		synced = false;
		if (options::ReadTags) ask_for_tags (playlist);
		iface->redraw(2);
		return 1;
	}
	else
	{
		iface->message ("File could not be read");
		iface->status ("");
		return 0;
	}

}

void Client::go_dir_up ()
{
	if (cwd.empty() || cwd == "/") return;
	auto i = cwd.rfind('/', cwd.length()-1);
	if (i == 0 || i == str::npos)
		go_to_dir("/");
	else
		go_to_dir(cwd.substr(0, i).c_str());
}

void Client::set_mixer (int val)
{
	val = CLAMP(0, val, 100);
	srv.send(CMD_SET_MIXER);
	srv.send(val);
}
void Client::adjust_mixer (int diff)
{
	set_mixer (get_mixer_value() + diff);
}

/* Recursively add the content of a directory to the playlist. */
void Client::add_to_plist (bool recursive)
{
	if (!iface->in_dir_plist()) return;

	auto r = iface->selection();
	if (r.first < 0) return;

	plist pl;
	for (int i = r.first; i <= r.second; ++i)
	{
		auto &item = *dir_plist.items[i];
		switch (item.type)
		{
			case F_SOUND: pl += item; break;
			case F_DIR: pl.add_directory(item.path, recursive); break;
			case F_PLAYLIST:
			{
				plist tmp;
				if (tmp.load_m3u(item.path)) pl += std::move(tmp);
				break;
			}
		}
	}
	if (pl.empty()) return;
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

	iface->left.move(REQ_DOWN);
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
		iface->message("You can only delete an item from the playlist.");
		return;
	}

	assert (playlist.size() > 0);
	auto r = iface->selection();
	int i = r.first, n = r.second-r.first+1;
	if (i < 0 || n <= 0) return;

	if (synced)
	{
		srv.send(CMD_PLIST_DEL);
		srv.send(i);
		srv.send(n);
	}
	else
	{
		playlist.remove(i, n);
		iface->redraw(2);
	}
	iface->select_song(i);
}

/* Select the file that is currently played. */
void Client::go_to_playing_file ()
{
	if (!iface->in_dir_plist() && synced)
	{
		int idx = iface->get_curr_index();
		if (idx >= 0)
		{
			iface->select_song(idx);
			return;
		}
	}

	str path = iface->get_curr_file(); if (path.empty()) return;
	if (!go_to_dir(containing_directory(path).c_str()))
	{
		iface->status("File not found!");
		return;
	}
	iface->select_path(path);
	iface->go_to_dir_plist();
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

/* Move the current playlist item (direction: -1 - up, +1 - down). */
void Client::move_item (int direction)
{
	if (iface->in_dir_plist()) {
		iface->message("You can move only playlist items.");
		return;
	}

	assert (direction == -1 || direction == 1);

	auto r = iface->selection();
	if (r.first < 0 || r.second >= playlist.size() ||
		r.first+direction < 0 || r.second+direction >= playlist.size())
	return;

	int i = (direction > 0 ? r.second : r.first) + direction;
	int j = (direction > 0 ? r.first : r.second);

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
	iface->move_selection(direction);
}

Client::Client(int sock, stringlist &args)
: srv(sock, false), synced(false)
, want_plist_update(false), want_state_update(false)
{
	logit ("Starting MOC Interface");

	/* Set locale according to the environment variables. */
	if (!setlocale(LC_CTYPE, "")) logit ("Could not set locale!");

	keys_init ();
	iface.reset(new Interface(*this, dir_plist, playlist));
	srv.send(CMD_GET_OPTIONS);

	if (options::ShowMixer)
	{
		srv.send(CMD_GET_MIXER_CHANNEL_NAME);
		iface->update_mixer_name(get_data_str());
		iface->update_mixer_value(get_mixer_value());
	}

	xsignal(SIGQUIT,  sig_quit);
	xsignal(SIGTERM,  sig_quit);
	xsignal(SIGHUP,   sig_quit);
	xsignal(SIGINT,   sig_interrupt);
	xsignal(SIGWINCH, sig_winch);

	const size_t nargs = args.size();
	bool want_sync = false;
	if (nargs == 0)
	{
		// start in current or music dir and with server playlist
		want_sync = true; // and leave cwd empty
	}
	else if (nargs > 1)
	{
		// start in current or music dir and with args as local playlist
		want_sync = false;
		for (str &arg : args)
		{
			if (is_url(arg.c_str())) {
				playlist += arg;
				continue;
			}
			str p = absolute_path(arg);
			if (is_dir (p.c_str()))
				playlist.add_directory(p, true);
			else if (is_plist_file(p.c_str())) {
				plist tmp; tmp.load_m3u(p);
				playlist += std::move(tmp);
			}
			else if (is_sound_file(p.c_str())) {
				playlist += p;
			}
		}
	}
	else
	{
		const str &arg = args[0];
		if (is_dir(arg.c_str())) {
			// start there, use server playlist
			set_cwd(arg);
			if (!go_to_dir(NULL)) interface_fatal("Directory can not be read: %s", arg.c_str());
			want_sync = true;
		}
		else if (is_plist_file (arg.c_str()))
		{
			// use as playlist (TODO: we should be playing this without
			// killing the server plist instead and then start in the
			// directory where arg is -- but for now do as for nargs > 1)
			want_sync = false;
			playlist.load_m3u(absolute_path(arg));
		}
		else
		{
			// go into file's directory and play it
			want_sync = true;
			set_cwd(containing_directory(arg));
			srv.send(CMD_PLAY);
			srv.send(-1);
			srv.send(absolute_path(arg));
		}
	}

	#define CHECK if(!go_to_dir(NULL)) cwd.clear()
	if (cwd.empty() && options::StartInMusicDir && is_dir(options::MusicDir.c_str())) {
		set_cwd(options::MusicDir); CHECK; }
	if (cwd.empty() && read_last_dir()) CHECK;
	if (cwd.empty()) { set_cwd("."); CHECK; }
	if (cwd.empty()) { cwd = options::Home; CHECK; }
	if (cwd.empty()) interface_fatal ("Can't enter any directory!");
	#undef CHECK

	if (want_sync)
	{
		srv.send(CMD_PLIST_GET);
		wait_for_data();
		srv.get(playlist);
		synced = true;
	}

	update_state ();

	if (synced)
	{
		int idx = iface->get_curr_index();
		if (idx >= 0) iface->select_song(idx);
	}

	ask_for_tags(playlist);

}

void Client::run()
{
	bool srv_event = false;
	while (true)
	{
		fd_set fds; FD_ZERO (&fds);
		int srv_sock = srv.fd();
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		timespec timeout = {0, srv_event ? 1 : 1000*1000*500}; // = {sec,nanosec}
		int n = pselect (srv_sock + 1, &fds, NULL, NULL, &timeout, NULL);
		if (n == -1 && !want_quit && errno != EINTR)
			interface_fatal ("pselect() failed: %s", xstrerror (errno));
		if (want_quit) break;

		if (n > 0 && FD_ISSET(srv_sock, &fds))
		{
			int type;
			if (srv.get_int_noblock(type))
			{
				handle_server_event(type);
				srv_event = true;
				continue; // handle all events before redrawing
			}
			else
				debug ("Getting event would block.");
		}
		srv_event = false;

		if (n > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
			iface->handle_input();
			Client::want_interrupt = false;
		}

		if (want_quit) break;

		if (want_resize)
		{
			iface->resize();
			want_resize = 0;
		}

		if (want_plist_update && synced)
		{
			srv.send(CMD_PLIST_GET);
			wait_for_data();
			want_plist_update = false;
			srv.get(playlist);
			ask_for_tags(playlist);
		}
		else want_plist_update = false;
		
		if (want_state_update)
		{
			update_state();
		}

		if (options::ShowMixer)
			iface->update_mixer_value(get_mixer_value());

		time_t curr_time = time(NULL);
		if (silent_seek_pos != -1 && silent_seek_key_last < curr_time)
		{
			seek (silent_seek_pos - iface->get_curr_time() - 1);
			silent_seek_pos = -1;
		}

		iface->draw();
	}
}

Client::~Client ()
{
	FILE *dir_file = fopen(options::config_file_path("last_directory").c_str(), "w");
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
	logit ("KEY EVENT: 0x%02x", (int)cmd);
	switch (cmd)
	{
		case KEY_CMD_QUIT_CLIENT: want_quit = 1; return;
		case KEY_CMD_QUIT:        want_quit = 2; return;

		case KEY_CMD_GO:
		{
			const plist_item *i = iface->sel_item();
			if (!i) break;
			if (i->type == F_SOUND || i->type == F_URL)
			{
				if (iface->in_dir_plist())
				{
					srv.send(CMD_PLAY);
					srv.send(-1);
					srv.send(i->path);
				}
				else
				{
					srv.send(CMD_PLAY);
					srv.send(iface->selected_song());
					if (!synced)
					{
						srv.send(playlist);
						synced = true;
					}
					else srv.send("");
				}
			}
			else if (i->type == F_DIR && iface->in_dir_plist()) {
				go_to_dir (i->path.c_str());
			}
			else if (i->type == F_PLAYLIST)
				go_to_playlist (i->path.c_str());
			break;
		}
		case KEY_CMD_STOP: srv.send(CMD_STOP); break;
		case KEY_CMD_NEXT:
		{
			if (iface->get_state() != STATE_STOP)
				srv.send(CMD_NEXT);
			else if (playlist.size()) {
				srv.send(CMD_PLAY);
				srv.send(-1);
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
			break;
		case KEY_CMD_TOGGLE_REPEAT:
			srv.send(CMD_SET_OPTION_REPEAT);
			srv.send(!options::Repeat);
			break;
		case KEY_CMD_TOGGLE_AUTO_NEXT:
			srv.send(CMD_SET_OPTION_AUTONEXT);
			srv.send(!options::AutoNext);
			break;
		case KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS:
			options::PlaylistFullPaths ^= 1;
			iface->redraw(2);
			break;
		case KEY_CMD_PLIST_ADD_FILE:
			add_to_plist (false);
			break;
		case KEY_CMD_PLIST_CLEAR:
			if (synced)
			{
				playlist.clear();
				synced = false;
			}
			else if (!playlist.empty())
			{
				playlist.clear();
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
			add_to_plist (true);
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
			if (options::MusicDir.empty()) {
				iface->message("ERROR: MusicDir not defined");
				return;
			}
			str music_dir = normalize_path(options::MusicDir);

			switch (plist_item::ftype(music_dir)) {
				case F_DIR: go_to_dir (music_dir.c_str()); break;
				case F_PLAYLIST: go_to_playlist (music_dir.c_str()); break;
				default: iface->message("ERROR: MusicDir is neither a directory nor a playlist!"); break;
			}
			break;
		}
		case KEY_CMD_PLIST_DEL:
			delete_item ();
			break;
		case KEY_CMD_GO_DIR_UP:
			if (iface->in_dir_plist()) go_dir_up ();
			break;
		case KEY_CMD_WRONG:
			iface->message("Bad command / key not bound to anything");
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
		case KEY_CMD_PLIST_MOVE_UP:   move_item (-1); break;
		case KEY_CMD_PLIST_MOVE_DOWN: move_item (+1); break;
		default:
			iface->message(format("BUG: invalid key command %d!", (int)cmd));
			break;
	}
}
