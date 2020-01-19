#pragma once
#include <functional>

#include "Util/Window.h"
#include "Views/Panel.h"
#include "Views/Menu.h"
#include "Views/FrameView.h"
#include "Views/InfoView.h"
#include "Views/Dialog.h"
class Client;
class plist;

typedef std::pair<int,int> Ratio;

class Interface
{
public:
	Interface(Client &client, plist &left, plist &right);

	void draw();
	void redraw(int k) { need_redraw = std::max(need_redraw, k); }
	void resize(); // Handle terminal size change.
	void handle_input(); // read the next key stroke
	bool handle_command(key_cmd cmd);

	bool in_dir_plist() const { return active == &left; }
	void go_to_dir_plist() { if (in_dir_plist()) return; active = &left; redraw(2); }

	void select_song(int i) { right.select_item(i); }
	bool select_path(const str &p) { go_to_dir_plist(); return left.select_path(p); }
	void move_selection(int dy) // moves the entire multi-selection!
	{
		active->sel += dy;
		redraw(2);
	}

	int selected_song() { assert(!in_dir_plist()); return right.xsel ? -1 : right.sel; }
	plist_item *sel_item() const
	{
		auto &m = *active;
		if (m.xsel || m.sel < 0 || m.sel >= m.items.size()) return NULL;
		return m.items.items[m.sel].get();
	}
	std::pair<int,int> selection() const // returns [min, max]
	{
		auto &m = *active;
		if (m.sel < 0) return std::make_pair(-1, -1);
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
	void confirm_quit(int i);

	// update and get info set by client:
	bool update_curr_file(const str &f, int idx);
	void drop_sync() { update_curr_file(curr_file, curr_idx); }

	str get_curr_file()  const { return curr_file; }
	int get_curr_index() const { return curr_idx; }

	int  get_total_time() const { return curr_tags ? curr_tags->time : 0; }
	void update_curr_tags(file_tags *t) { curr_tags.reset(t); redraw(1); }
	
	Window    win;
	Panel     left, right, *active;
	Client   &client;
	Menu      menu;
	InfoView  info;
	FrameView frame;
	std::unique_ptr<Dialog> dlg;

private:
	friend class InfoView;
	friend class FrameView;
	friend class Dialog;
	friend class Menu;
	str curr_file; int curr_idx;
	std::unique_ptr<file_tags> curr_tags;
	int left_total, right_total;

	void cycle_layouts();
	void clear_dialog()
	{
		if (dlg) redraw(2);
		if (dragging == dlg.get()) dragging = NULL;
		dlg.reset(nullptr);
	}
	bool can_tag() const;

	View *dragging;

	int need_redraw; // 1: info only, 2: everything

	std::queue<str> messages;
	time_t message_display_start; // for current message, if any
	str status_msg;
};
