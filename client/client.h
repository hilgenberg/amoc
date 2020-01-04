#pragma once
#include "interface.h"
#include "../lists.h"
#include "../files.h"
#include "../playlist.h"
#include "keys.h"
#include "../protocol.h"

class Client
{
public:
	Client(int sock, stringlist &args);
	~Client();
	void run ();

	plist playlist, dir_plist;
	str cwd; // current position of dir_plist
	bool synced; // is our playlist synced with the server's?

	void handle_command(key_cmd cmd); // does everything that's not purely UI

	static volatile int  want_quit; // 1=quit client, 2=quit server
	static volatile bool want_interrupt;
	static volatile bool want_resize;

private:
	Socket srv;
	std::unique_ptr<Interface> iface;
	
	int silent_seek_pos = -1; /* Silent seeking - where we are in seconds. -1 - no seeking. */
	time_t silent_seek_key_last = (time_t)0; /* when the silent seek key was last used */
	time_t last_menu_move_time = (time_t)0; /* When the menu was last moved (arrow keys, page up, etc.) */

	void wait_for_data();
	void handle_server_event(int type);
	int  get_data_int () { wait_for_data(); return srv.get_int (); }
	bool get_data_bool() { wait_for_data(); return srv.get_bool(); }
	str  get_data_str () { wait_for_data(); return srv.get_str (); }
	void send_tags_request (const str &file);

	void get_server_options();
	int get_mixer_value();
	int get_channels();
	int get_rate();
	int get_bitrate();
	int get_avg_bitrate();
	int get_curr_time();
	str get_curr_file();
	PlayState get_state();
	void update_mixer_name();
	
	void set_cwd(const str &path);
	void set_start_dir ();
	bool read_last_dir ();
	void ask_for_tags (const plist &plist);
	void interface_message (const char *format, ...);
	void update_item_tags (plist &plist, const int num, file_tags *tags);
	void update_curr_tags ();
	void update_state ();
	void forward_playlist ();
	bool go_to_dir (const char *dir);
	int  go_to_playlist (const char *file);
	void go_file ();
	void enter_first_dir ();
	void process_args (stringlist &args);
	void load_playlist ();
	void go_dir_up ();
	void send_playlist (plist &plist, bool clear);
	void play_it(const str &file);
	void set_mixer (int val);
	void adjust_mixer (const int diff);
	void add_dir_plist ();
	void add_file_plist ();
	void set_rating (int r);
	void switch_read_tags ();
	void seek (const int sec) { srv.send(CMD_SEEK); srv.send(sec); }
	void jump_to (const int sec) { srv.send(CMD_JUMP_TO); srv.send(sec); }
	void seek_to_percent (int percent) { srv.send(CMD_JUMP_TO); srv.send(-percent); }
	void delete_item ();
	void go_to_playing_file ();
	void seek_silent (int dt);
	void move_item (int direction);

};

inline int user_wants_interrupt ()
{
	return Client::want_interrupt;
}
