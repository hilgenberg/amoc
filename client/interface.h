#pragma once
#include <ncurses.h>
#include <wctype.h>
#include <wchar.h>
#include <functional>

#include "keys.h"
#include "../playlist.h"
#include "menu.h"
#include "Rect.h"
#include "../server/server.h" // PlayState
class Client;
class plist;

extern void interface_error (const char *msg);
extern void interface_fatal (const char *format, ...);
typedef std::pair<int,int> Ratio;

class Interface
{
public:
	Interface(Client &client, plist &left, plist &right);
	~Interface();
	void draw(bool force=false);
	void redraw(int k) { need_redraw = std::max(need_redraw, k); }
	void resize(); // Handle terminal size change.
	void handle_input(); // read the next key stroke
	bool handle_click(int x, int y, bool dbl);
	bool handle_drag(int x, int y, int seq);
	bool handle_scroll(int x, int y, int dy);

	bool in_dir_plist() const { return active_menu == 0; }
	void go_to_dir_plist() { if (in_dir_plist()) return; active_menu = 0; redraw(2); }

	void select_song(int i) { right.select_item(i); }
	bool select_path(const str &p) { go_to_dir_plist(); return left.select_path(p); }
	void move_selection(int dy) // moves the entire multi-selection!
	{
		auto &panel = *menus[active_menu];
		panel.sel += dy;
		redraw(2);
	}

	int  selected_song() { assert(!in_dir_plist()); return right.xsel ? -1 : right.sel; }
	plist_item *sel_item() { auto &m = *menus[active_menu]; return (m.sel < 0 || m.sel >= m.items.size()) ? NULL : m.items.items[m.sel].get(); }
	std::pair<int,int>  selection() // returns [min, max]
	{
		auto &m = *menus[active_menu];
		return m.xsel < 0 ? std::make_pair(m.sel+m.xsel, m.sel)
		                  : std::make_pair(m.sel, m.sel+m.xsel);
	}

	void status(const str &s)
	{
		if (s == status_msg) return;
		redraw(2); // may need to redraw the frame and total time
		status_msg = s;
	}

	void error_message(const str &msg) { message(str("ERROR: " + msg)); }
	void message(const str &msg)
	{
		if (msg.empty()) return;
		if (!messages.empty() && messages.back() == msg)
		{
			// avoid collecting hours of displaytime on the same message
			if (messages.size() == 1)
			{
				message_display_start = 0;
				redraw(1);
			}
		}
		else
		{
			if (messages.empty()) redraw(1);
			messages.push(msg);
		}
	}

	// update and get info set by client:
	bool update_curr_file(const str &f, int idx)
	{
		if (curr_file==f && curr_idx==idx) return false;
		curr_file = f; curr_idx = idx;
		curr_tags.reset(nullptr);
		redraw(2);
		return true;
	}
	int get_curr_index() const { return curr_idx; }
	void update_curr_tags(file_tags *t) { curr_tags.reset(t); redraw(1); }
	int  get_total_time() const { return curr_tags ? curr_tags->time : 0; }
	str  get_curr_file() const { return curr_file; }
	#define UPD(T,x)\
		void update_ ## x(T v) { if (x==v) return; x=v; redraw(1); } \
		T get_ ## x() const { return x; }
	UPD(int, bitrate)
	UPD(int, avg_bitrate)
	UPD(int, rate)
	UPD(int, curr_time)
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
	str curr_file; int curr_idx;
	std::unique_ptr<file_tags> curr_tags;
	int bitrate, avg_bitrate; // in kbps
	int rate;		  // in kHz
	int curr_time;
	int channels;
	PlayState state; // STATE_(PLAY | STOP | PAUSE)
	str mixer_name;
	int mixer_value;

	void cycle_layouts();
	int  drag0; // last x or y value or -1 if not dragging anything
	bool dragTime; // dragging in the time bar?

	int need_redraw; // 1: info only, 2: everything

	std::queue<str> messages;
	time_t message_display_start; // for current message, if any
	str status_msg;

	void prompt(const str &prompt, const str &resp0, int curs0, std::function<void(void)> callback);
	bool prompting;
	str prompt_str, response; int cursor, hscroll; // libreadline?
	std::function<void(void)> callback;
};
