#include "interface.h"
#include "client.h"
#include "Util/themes.h"
#include "Util/keys.h"

Interface::Interface(Client &client, plist &pl1, plist &pl2)
: client(client)
, left(*this, pl1)
, right(*this, pl2)
, active(&right)
, need_redraw(2)
, left_total(-1), right_total(-1)
, message_display_start(0)
, menu(*this)
, frame(*this)
, info(*this)
, dragging(NULL)
{
	mousemask(REPORT_MOUSE_POSITION | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | 
	          BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON4_PRESSED|BUTTON5_PRESSED, NULL);
}

int Interface::get_total_time() const
{
	return client.tags.get_time(curr_file);
}

void Interface::resize ()
{
	win.resize();
	redraw(3);
}

void Interface::cycle_layouts()
{
	int tmp = options::layout; ++tmp %= 3;
	options::layout = (Layout)tmp;
	redraw(3);
}

bool Interface::update_curr_file(const str &f, int idx)
{
	if (!client.synced) idx = -1;
	if (curr_file==f && curr_idx==idx) return false;
	curr_file = f; curr_idx = idx;
	redraw(2);
	return true;
}

bool Interface::can_tag() const
{
	auto sel = selection();
	const int n = (sel.second+1-sel.first);
	if (sel.first < 0 || n <= 0) return false;
	auto &pl = active->items;
	for (int i = sel.first; i <= sel.second; ++i)
	{
		auto &it = pl[i];
		if (!it.can_tag()) return false;
	}
	return true;
}
bool Interface::can_mv() const
{
	auto sel = selection();
	const int n = (sel.second+1-sel.first);
	if (sel.first < 0 || n <= 0) return false;
	if (in_dir_plist() && sel.first == 0 && client.cwd != "/") return false;
	auto &pl = active->items;
	std::set<str> dirs;
	for (int i = sel.first; i <= sel.second; ++i)
	{
		auto &it = pl[i];
		if (it.type == F_URL) return false;
		for (auto &d : dirs)
			if (has_prefix(it.path, d, false)) return false;
		if (it.type == F_DIR) dirs.insert(it.path + "/");
	}
	return true;
}
bool Interface::can_rm() const
{
	// only handle files for now
	auto sel = selection();
	const int n = (sel.second+1-sel.first);
	if (sel.first < 0 || n <= 0) return false;
	auto &pl = active->items;
	for (int i = sel.first; i <= sel.second; ++i)
	{
		auto &it = pl[i];
		if (it.type == F_URL || it.type == F_DIR) return false;
	}
	return true;
}

bool Interface::handle_command(key_cmd cmd)
{
	switch (cmd)
	{
		case KEY_CMD_MENU_DOWN:  active->move_selection(REQ_DOWN);   redraw(2); break;
		case KEY_CMD_MENU_UP:    active->move_selection(REQ_UP);     redraw(2); break;
		case KEY_CMD_MENU_NPAGE: active->move_selection(REQ_PGDOWN); redraw(2); break;
		case KEY_CMD_MENU_PPAGE: active->move_selection(REQ_PGUP);   redraw(2); break;
		case KEY_CMD_MENU_FIRST: active->move_selection(REQ_TOP);    redraw(2); break;
		case KEY_CMD_MENU_LAST:  active->move_selection(REQ_BOTTOM); redraw(2); break;
		case KEY_CMD_MENU_EXTEND_DOWN: active->move_selection(REQ_XDOWN); redraw(2); break;
		case KEY_CMD_MENU_EXTEND_UP:   active->move_selection(REQ_XUP);   redraw(2); break;
		case KEY_CMD_TOGGLE_MENU: active = (active==&left ? &right : &left); redraw(2); break;
		case KEY_CMD_REFRESH: redraw(3); break;
		case KEY_CMD_TOGGLE_LAYOUT: cycle_layouts(); break;

		case KEY_CMD_MENU: menu.active = !menu.active; redraw(2); break;

		case KEY_CMD_ADD_STREAM:
			dlg.reset(new Dialog(*this, Dialog::ADD_URL));
			redraw(2);
			break;

		case KEY_CMD_PLIST_SAVE:
			if (!client.playlist.size())
				error ("The playlist is empty.");
			else
				dlg.reset(new Dialog(*this, Dialog::SAVE_PLIST));
			break;

		case KEY_CMD_TAG_ARTIST: if (can_tag()) dlg.reset(new Dialog(*this, Dialog::EDIT_ARTIST)); break;
		case KEY_CMD_TAG_ALBUM:  if (can_tag()) dlg.reset(new Dialog(*this, Dialog::EDIT_ALBUM)); break;
		case KEY_CMD_TAG_TITLE:  if (can_tag()) dlg.reset(new Dialog(*this, Dialog::EDIT_TITLE)); break;

		case KEY_CMD_TAG_ADD_NUMBERS:
		case KEY_CMD_TAG_DEL_NUMBERS:
		{
			if (!can_tag()) break;
			auto sel = selection(); assert(sel.first >= 0);
			auto &pl = active->items;
			for (int i = sel.first; i <= sel.second; ++i)
			{
				auto &it = pl[i]; assert(it.can_tag());
				int k = (cmd == KEY_CMD_TAG_DEL_NUMBERS ? 0 : i-sel.first+1);
				client.change_track(it, k);
			}
			redraw(2);
			break;
		}

		case KEY_CMD_FILES_RM: if (can_rm()) dlg.reset(new Dialog(*this, Dialog::FILES_RM)); break;
		case KEY_CMD_FILES_MV: if (can_mv()) dlg.reset(new Dialog(*this, Dialog::FILES_MV)); break;

		/*case KEY_CMD_MENU_SEARCH:
			prompt("SEARCH", NULL, ...);
			iface_make_entry (ENTRY_SEARCH);
			break;*/
		/*case KEY_CMD_GO_DIR:
			prompt("GO", NULL, ...);
			break;
		case KEY_CMD_GO_URL:
			prompt("URL", NULL, ...);
			iface_make_entry (ENTRY_GO_URL);
			break;*/
		default:
			return client.handle_command(cmd);
	}
	return true;
}

void Interface::confirm_quit(int i)
{
	if (client.tags.changes.empty())
	{
		client.want_quit = i;
		return;
	}
	dlg.reset(new Dialog(*this, i == 2 ? Dialog::CONFIRM_QUIT : Dialog::CONFIRM_QUIT_CLIENT));
}

void Interface::handle_input()
{
	wchar_t c = 0; // regular key
	int     f = 0; // function key

	WINDOW *w = win.WIN();

	wint_t ch = wgetch(w);
	if (ch == (wint_t)ERR) interface_fatal ("wgetch() failed!");

	if (ch < 32 && ch != '\n' && ch != '\t' && ch != KEY_ESCAPE)
		f = ch; /* Unprintable, generally control sequences */
	else if (ch == 0x7f)
		f = KEY_BACKSPACE; /* Workaround for backspace on many terminals */
	else if (ch < 255)
	{
		/* Regular char */
		ungetch (ch);
		if (wget_wch(w, &ch) == ERR) interface_fatal ("wget_wch() failed!");

		/* Recognize meta sequences */
		if (ch == KEY_ESCAPE) {
			int meta = wgetch (w);
			if (meta != ERR) ch = meta | META_KEY_FLAG;
			f = ch;
		}
		else c = ch;
	}
	else f = ch;

	if (f == KEY_RESIZE)
	{
		resize();
	}
	else if (f == KEY_MOUSE)
	{
		MEVENT ev;
		while (getmouse(&ev) == OK)
		{
			// if dragging does not work, the following might help:
			// $ export TERM=xterm-1003

			//#define MOUSE_DEBUG(T) status(format("MOUSE %s (%d,%d) %d", T, ev.x, ev.y, (int)ev.bstate))
			#define MOUSE_DEBUG(...) 

			if ((ev.bstate & BUTTON1_RELEASED))
			{
				MOUSE_DEBUG("UP");
				if (dragging) dragging->finish_drag(ev.x, ev.y);
				else if (!dlg && menu.active) menu.finish_drag(ev.x, ev.y);
				dragging = NULL;
			}
			else if ((ev.bstate & REPORT_MOUSE_POSITION))
			{
				MOUSE_DEBUG(dragging ? "DRAG" : "MOVE");
				if (dragging) dragging->handle_drag(ev.x, ev.y);
				else if (!dlg && menu.active) menu.handle_drag(ev.x, ev.y);
			}
			else if ((ev.bstate & BUTTON1_PRESSED))
			{
				MOUSE_DEBUG("DOWN");
				if (dlg &&dlg->start_drag(ev.x, ev.y)) dragging = dlg.get(); 
				else if ( menu.start_drag(ev.x, ev.y)) dragging = &menu; 
				else if ( info.start_drag(ev.x, ev.y)) dragging = &info; 
				else if (frame.start_drag(ev.x, ev.y)) dragging = &frame;
				else                                   dragging = NULL;
			}
			else if ((ev.bstate & (BUTTON1_CLICKED|BUTTON1_DOUBLE_CLICKED)))
			{
				MOUSE_DEBUG("CLICK");
				dragging = NULL;
				bool dbl = ev.bstate & BUTTON1_DOUBLE_CLICKED;
				(dlg && dlg->handle_click(ev.x, ev.y, dbl)) ||
				menu .handle_click(ev.x, ev.y, dbl) ||
				frame.handle_click(ev.x, ev.y, dbl) ||
				info .handle_click(ev.x, ev.y, dbl);
			}
			else if ((ev.bstate & (BUTTON4_PRESSED|BUTTON5_PRESSED)))
			{
				bool up = ev.bstate & BUTTON4_PRESSED;
				MOUSE_DEBUG(up ? "SCROLL UP" : "SCROLL DOWN");
				int dy = up ? -1 : 1;
				(dlg && dlg->handle_scroll(ev.x, ev.y, dy)) ||
				menu .handle_scroll(ev.x, ev.y, dy) ||
				frame.handle_scroll(ev.x, ev.y, dy) ||
				info .handle_scroll(ev.x, ev.y, dy);
			}
			else
			{
				MOUSE_DEBUG("GARBAGE");
			}
		}
	}
	else if (dlg)
	{
		dlg->handle_key(c, f);
	}
	else if (menu.handle_key(c, f))
	{
		return;
	}
	else
	{
		//status(format("KEY %d %d", (int)c, (int)f));
		auto cmd = get_key_cmd (CON_PANEL, c, f);
		handle_command(cmd);
	}
}

void Interface::draw()
{
	if (user_wants_interrupt() && dlg)
	{
		clear_dialog();
	}

	if (!messages.empty())
	{
		time_t t = time(NULL);
		if (message_display_start == 0)
		{
			message_display_start = t;
			redraw(1);
		}
		else if (t > message_display_start + options::MessageLingerTime)
		{
			messages.pop();
			message_display_start = (messages.empty() ? 0 : t);
			redraw(1);
		}
	}

	if (!need_redraw) return;

	const int W = COLS, H = LINES;
	if (W < 8 || H < 6)
	{
		str s("...TERMINAL TOO SMALL...");
		win.clear();
		win.color(CLR_MESSAGE);

		if (H > W) for (int y = 0; y < H; ++y)
		{
			int i = y-(H-1)/2 + s.length()/2;
			if (i < 0 || i >= s.length()) continue;
			win.put(y, (W-1)/2, s[i]);
		}
		else for (int x = 0; x < W; ++x)
		{
			int i = x-(W-1)/2 + s.length()/2;
			if (i < 0 || i >= s.length()) continue;
			win.put((H-1)/2, x, s[i]);
		}
		need_redraw = 0;
		menu.active = false;
		win.flush();
		return;
	}

	if (info.get_state() == STATE_STOP)
	{
		curr_file.clear();
		curr_idx = -1;
	}
	
	// make sure there is always a selection
	if (active->items.empty())
	{
		if (!left.items.empty()) active = &left;
		if (!right.items.empty()) active = &right;
	}
	auto &am = *active;
	if (am.sel == -1 && am.mark == -1 && !am.items.empty())
		am.sel = 0;

	if (need_redraw > 1) // more than just info changed?
	{
		win.clear();
		Rect r1, r2; // for left+right = dir+plist
		switch (options::layout)
		{
			case HSPLIT:
			{
				auto &r = options::hsplit; int a = MAX(0, r.first), b = MAX(0, r.second);
				if (a+b == 0) a = b = 1;
				int w1 = CLAMP(0, std::round((W-4)*(double)b/(a+b)), W-4);
				int w2 = (W-4)-w1;
				r1.set(0, 0, w1+2, H-3);
				r2.set(W-(w2+2), 0, w2+2, H-3);
				break;
			}
			case VSPLIT:
			{
				auto &r = options::vsplit; int a = MAX(0, r.first), b = MAX(0, r.second);
				if (a+b == 0) a = b = 1;
				int h1 = CLAMP(0, std::round((H-6)*(double)b/(a+b)), H-6);
				int h2 = (H-6)-h1;
				r2.set(0, 0, W, h2+2);
				r1.set(0, h2+1, W, h1+2);
				break;
			}
			case SINGLE:
				r1.set(0, 0, W, H-3);
				r2 = r1;
				break;
		}
		left.bounds = r1.inset(1);
		right.bounds = r2.inset(1);

		left.mark_path(curr_file);
		right.mark_item(curr_idx);

		Panel *other = (active == &left ? &right : &left);
		other->set_active(false); if (options::layout != SINGLE) other->draw();
		active->set_active(true); active->draw();

		left_total = left.items.total_time();
		right_total = right.items.total_time();
	}

	frame.draw();
	info.draw();
	menu.draw();

	// dlg must be last so the cursor stays in the right place
	if (dlg) dlg->draw(); else curs_set(0);

	need_redraw = 0;
	win.flush();
}
