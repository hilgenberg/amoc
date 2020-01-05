#pragma once
#include <ncurses.h>
#include <wctype.h>
#include <wchar.h>
#include <functional>

#include "../lists.h"
#include "../files.h"
#include "keys.h"
#include "../playlist.h"
#include "menu.h"
#include "Rect.h"
#include "../server/server.h" // PlayState
class Client;
class plist;

extern void interface_error (const char *msg);
extern void interface_fatal (const char *format, ...);

class Interface
{
public:
	Interface(Client &client, plist &left, plist &right);
	~Interface();
	void draw(bool force=false);
	void redraw(int k) { need_redraw = std::max(need_redraw, k); }
	void resize(); // Handle terminal size change.
	void handle_input(); // read the next key stroke
	
	bool in_dir_plist() const { return active_menu == 0; }
	plist &sel_plist() { return menus[active_menu]->items; }
	plist_item *sel_item() { auto &m = *menus[active_menu]; return m.sel < 0 ? NULL : m.items.items[m.sel].get(); }
	bool sel_item(const plist_item *what, int where); // where: -1=active menu, 0=left, 1=right
	int sel_index(int where=-1) { auto &m = *menus[where<0 ? active_menu : where]; return m.sel; }
	void move_down();

	void status(const str &s) { status_msg = s; redraw(1); }
	void error_message(const str &msg) { message(str("ERROR: " + msg)); }
	void message(const str &msg) { if (messages.empty()) redraw(1); messages.push(msg); }
	bool select_path(const str &p) { return left.select_path(p); }

	// update and get info set by client:
	void update_curr_file(const str &f) { if (curr_file==f) return; curr_file = f; redraw(2); }
	void update_curr_tags(file_tags *t) { curr_tags.reset(t); redraw(1); }
	str  get_curr_file() const { return curr_file; }
	#define UPD(T,x)\
		void update_ ## x(T v) { if (x==v) return; x=v; redraw(1); } \
		T get_ ## x() const { return x; }
	UPD(int, bitrate)
	UPD(int, avg_bitrate)
	UPD(int, rate)
	UPD(int, curr_time)
	UPD(int, total_time)
	UPD(int, channels)
	UPD(PlayState, state)
	UPD(str, mixer_name)
	UPD(int, mixer_value)
	#undef UPD

	WINDOW *window() { return win; }
	str cwd() const;

	WINDOW *win;
	menu    left, right, *menus[2];
	int     active_menu;
	Client &client;

private:
	str curr_file;
	std::unique_ptr<file_tags> curr_tags;
	int bitrate, avg_bitrate; // in kbps
	int rate;		  // in kHz
	int curr_time, total_time;
	int channels;
	PlayState state; // STATE_(PLAY | STOP | PAUSE)
	str mixer_name;
	int mixer_value;

	enum Layout { HSPLIT=0, VSPLIT=1, SINGLE=2 };
	Layout layout;
	void cycle_layouts();

	int need_redraw; // 1: info only, 2: everything

	std::queue<str> messages;
	time_t message_display_start; // for current message, if any
	str status_msg;

	void prompt(const str &prompt, stringlist *history, std::function<void(void)> callback);
	bool prompting;
	str prompt_str, response; int cursor, hscroll; // libreadline?
	std::function<void(void)> callback;
};
