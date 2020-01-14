#include <locale.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/select.h>

#include "interface.h"
#include "client.h"
#include "utf8.h"
#include "Panel.h"
#include "themes.h"
#include "keys.h"
#include "../playlist.h"
#include "../protocol.h"
#include "../server/input/decoder.h"
#include "../server/output/softmixer.h"
#include "../server/ratings.h"

Interface::Interface(Client &client, plist &pl1, plist &pl2)
: client(client)
, left(this, pl1)
, right(this, pl2)
, active(&right)
, prompting(false)
, need_redraw(2)
, bitrate(-1), avg_bitrate(-1), rate(-1)
, curr_time(0), channels(0)
, state(STATE_STOP), mixer_value(-1)
, message_display_start(0)
, menu(*this)
, frame(*this)
, info(*this)
, dragging(NULL)
{
	mousemask(REPORT_MOUSE_POSITION | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | 
	          BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON4_PRESSED|BUTTON5_PRESSED, NULL);
}

void Interface::resize ()
{
	win.resize();
	redraw(2);
}

void Interface::cycle_layouts()
{
	++(int&)options::layout;
	(int&)options::layout %= 3;
	redraw(2);
}

void Interface::prompt(const str &prompt, const str &s0, int cur0, std::function<void(void)> cb)
{
	prompting = true;
	prompt_str = prompt;
	response = s0;
	cursor = hscroll = cur0;
	callback = cb;
	redraw(1);
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
		case KEY_CMD_REFRESH: redraw(2); break;
		case KEY_CMD_TOGGLE_LAYOUT: cycle_layouts(); break;

		case KEY_CMD_MENU: menu.active = !menu.active; redraw(2); break;

		/*case KEY_CMD_ADD_STREAM:
			prompt("ADD URL", NULL, ...);
			break;
		case KEY_CMD_MENU_SEARCH:
			prompt("SEARCH", NULL, ...);
			iface_make_entry (ENTRY_SEARCH);
			break;*/
		case KEY_CMD_PLIST_SAVE:
			if (!client.playlist.size())
				error ("The playlist is empty.");
			else
			{
				str p0 = client.cwd;
				if (!p0.empty() && p0.back() != '/') p0 += "/";
				p0 += ".m3u";
				prompt("SAVE PLAYLIST", p0, p0.length()-4, [this](){
					if (response.empty()) return;
					str fn = response;
					if (file_exists(fn.c_str())) {
						prompt("File exists, overwrite (y/n)?", "", 0, [this,fn](){
							if (response == "y")
							{
								status("Saving the playlist...");
								client.playlist.save(fn.c_str());
								status("Playlist saved.");
							}
							else
								status("Aborted.");
						});
					}
					else
					{
						status("Saving the playlist...");
						client.playlist.save(fn.c_str());
						client.handle_command(KEY_CMD_RELOAD);
						status("Playlist saved.");
					}
				});
			}
			break;
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
				else if (menu.active) menu.finish_drag(ev.x, ev.y);
				dragging = NULL;
			}
			else if ((ev.bstate & REPORT_MOUSE_POSITION))
			{
				MOUSE_DEBUG(dragging ? "DRAG" : "MOVE");
				if (dragging) dragging->handle_drag(ev.x, ev.y);
				else if (menu.active) menu.handle_drag(ev.x, ev.y);
			}
			else if ((ev.bstate & BUTTON1_PRESSED))
			{
				MOUSE_DEBUG("DOWN");
				if      ( menu.start_drag(ev.x, ev.y)) dragging = &menu; 
				else if ( info.start_drag(ev.x, ev.y)) dragging = &info; 
				else if (frame.start_drag(ev.x, ev.y)) dragging = &frame;
				else                                   dragging = NULL;
			}
			else if ((ev.bstate & (BUTTON1_CLICKED|BUTTON1_DOUBLE_CLICKED)))
			{
				MOUSE_DEBUG("CLICK");
				dragging = NULL;
				bool dbl = ev.bstate & BUTTON1_DOUBLE_CLICKED;
				menu .handle_click(ev.x, ev.y, dbl) ||
				frame.handle_click(ev.x, ev.y, dbl) ||
				info .handle_click(ev.x, ev.y, dbl);
			}
			else if ((ev.bstate & (BUTTON4_PRESSED|BUTTON5_PRESSED)))
			{
				bool up = ev.bstate & BUTTON4_PRESSED;
				MOUSE_DEBUG(up ? "SCROLL UP" : "SCROLL DOWN");
				int dy = up ? -1 : 1;
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
	else if (menu.active)
	{
		auto cmd = get_key_cmd (CON_MENU, c, f);
		if (cmd != KEY_CMD_WRONG)
		{
			if (!menu.handle_command(cmd)) handle_command(cmd);
		}
		else
		{
			cmd = get_key_cmd (CON_PANEL, c, f);
			if (cmd != KEY_CMD_WRONG && cmd != KEY_CMD_MENU)
			{
				handle_command(KEY_CMD_MENU);
			}
			handle_command(cmd);
		}
	}
	else if (prompting)
	{
		redraw(1);
		if (c == '\n')
		{
			prompting = false;
			callback();
			return;
		}
		else if (c)
		{
			cursor += strins(response, cursor, c);
			return;
		}

		switch (f)
		{
			case KEY_LEFT:  --cursor; return;
			case KEY_RIGHT: ++cursor; return;
			case KEY_HOME: cursor = 0; return;
			case KEY_END: cursor = strwidth(response); return;

			case KEY_BACKSPACE: if (cursor) strdel(response, --cursor);  return;
			case KEY_DC: if (cursor) strdel(response, cursor); return;
		}
		key_cmd cmd = get_key_cmd (CON_ENTRY, c, f);
		switch (cmd)
		{
			case KEY_CMD_CANCEL: prompting = false; return;
			case KEY_CMD_HISTORY_UP: /*TODO*/ return;
			case KEY_CMD_HISTORY_DOWN: /*TODO*/ return;
			case KEY_CMD_DELETE_START: strdel(response, 0, cursor); cursor = 0; return;
			case KEY_CMD_DELETE_END: strdel(response, cursor, strwidth(response)); return;
		}
	}
	else
	{
		//status(format("KEY %d %d", (int)c, (int)f));
		auto cmd = get_key_cmd (CON_PANEL, c, f);
		handle_command(cmd);
	}
}

void Interface::draw(bool force)
{
	if (user_wants_interrupt() && prompting)
	{
		prompting = false;
		redraw(1);
	}

	if (force) need_redraw = 2;

	if (!messages.empty())
	{
		time_t t = time(NULL);
		if (message_display_start == 0)
		{
			message_display_start = t;
			need_redraw = std::max(need_redraw, 1);
		}
		else if (t > message_display_start + options::MessageLingerTime)
		{
			messages.pop();
			message_display_start = (messages.empty() ? 0 : t);
			need_redraw = std::max(need_redraw, 1);
		}
	}

	if (!need_redraw) return;

	// make sure there is always a selection
	if (active->items.empty())
	{
		if (!left.items.empty()) active = &left;
		if (!right.items.empty()) active = &right;
	}
	auto &am = *active;
	if (am.sel == -1 && am.mark == -1 && !am.items.empty())
		am.sel = 0;

	const auto frame_color = dragging==&frame ? CLR_PANEL_FILE_SELECTED : CLR_FRAME;

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

	if (state == STATE_STOP)
	{
		curr_file.clear();
		curr_tags.reset(nullptr);
		bitrate = avg_bitrate = rate = 0;
		curr_time = 0;
		channels = 0;
	}
	
	if (need_redraw > 1) // tags or sizes changed?
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

		frame.draw();
	}

	//--- Info area ---------------------------------------------------------------------
	const int total_time = curr_tags ? curr_tags->time : 0;

	// status message
	if (!status_msg.empty())
	{
		int w = 0, sw = (int)status_msg.length();

		if (options::layout == HSPLIT)
		{
			if (left.bounds.w >= 30) w = left.bounds.w - 15;
			if (w < sw) w = left.bounds.w - 2;
			if (w < sw && right.bounds.w >= 30) w = W - 17;
		}
		else
		{
			if (w < sw && W >= 32) w = W - 17;
		}
	
		if (w < sw) w = W - 4;
		if (w < sw) sw = w;

		win.color(frame_color);
		win.moveto(H-4, 1);
		win.hl(w+2); // overwrite playlist times if needed
		int x0 = 1+(w-sw)/2;
		win.put(H-4, x0, ' '); //win.rtee);
		win.put(H-4, x0+sw+1, ' '); //win.ltee);

		win.color(CLR_STATUS);
		win.moveto(H-4, x0+1);
		win.field(status_msg, sw);
	}

	if (total_time <= 0 || options::TimeBarLine.empty())
	{
		win.moveto(H-1, 1);
		win.color(frame_color);
		win.hl(W-2);
	}

	info.draw();

	if (menu.active) menu.draw();

	need_redraw = 0;
	win.flush();
}
