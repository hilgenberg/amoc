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
#include "../rcc.h"
#include "../server/input/decoder.h"
#include "../server/output/softmixer.h"
#include "../server/ratings.h"

// width of the toggles for shuffle, repeat, ...
static constexpr int w_toggles = 6+3+7+6+4+5*2+4*1;

str Interface::cwd() const { return client.cwd; }

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
, drag0(-1)
{
	#define MOUSEMASK (REPORT_MOUSE_POSITION | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | \
		BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON4_PRESSED|BUTTON5_PRESSED)
	mousemask(MOUSEMASK, NULL);
	// leave defined, it's used below
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

#define CURR_RATIO (options::layout == HSPLIT ? options::hsplit : options::vsplit)

bool Interface::handle_click(int x, int y, bool dbl)
{
	if (options::layout != SINGLE && left.bounds.contains(x,y))
	{
		if (active == &right) { active = &left; redraw(2); }
		left.handle_click(x, y, dbl);
		return true;
	}
	else if (options::layout != SINGLE && right.bounds.contains(x,y))
	{
		if (active == &left) { active = &right; redraw(2); }
		right.handle_click(x, y, dbl);
		return true;
	}
	else if (options::layout != SINGLE && (left.bounds+right.bounds).contains(x,y))
	{
		if (dbl)
		{
			CURR_RATIO = std::make_pair(1,1);
			redraw(2);
			return true;
		}
	}
	else if (options::layout == SINGLE && active->bounds.contains(x,y))
	{
		active->handle_click(x, y, dbl);
		redraw(2);
		return true;
	}
	// TODO: middle mouse button to cd..
	// TODO: drag&drop to sort the playlist
	// TODO: shift-click, ctrl-click
	else
	{
		const int W = COLS, H = LINES;
		const int total_time = curr_tags ? curr_tags->time : 0;

		// time bar --> seek
		if (y == H-1 && x >= 1 && x <= W-2 && W > 8)
		{
			if (total_time <= 0 || options::TimeBarLine.empty()) return false;
			client.jump_to((x-1)*total_time/(W-3));
			return true;
		}

		// play/pause state --> toggle play/pause
		if (y == H-3 && x > 0 && x < 4)
		{
			client.handle_command(KEY_CMD_PAUSE);
			return true;
		}

		// file --> show file in dir list
		if (dbl && y == H-3 && x >= 4 && x < W-1)
		{
			go_to_dir_plist();
			client.handle_command(KEY_CMD_GO_TO_PLAYING_FILE);
			return true;
		}

		if (!prompting && y == H-2 && W >= w_toggles+2 && x >= W-w_toggles-1 && x <= W-2)
		{
			x -= W-w_toggles-1;
			#define CHK(len, cmd) if (x >= 0 && x < (len)+2) { client.handle_command(cmd); return true; } x -= (len)+3
			CHK(6,KEY_CMD_TOGGLE_MAKE_MONO); // STEREO
			x -= 3+3; // NET
			CHK(7,KEY_CMD_TOGGLE_SHUFFLE); // SHUFFLE
			CHK(6,KEY_CMD_TOGGLE_REPEAT); // REPEAT
			CHK(4,KEY_CMD_TOGGLE_AUTO_NEXT); // NEXT
			#undef CHK
			return true;
		}
	}

	return false;
}

bool Interface::handle_scroll(int x, int y, int dy)
{
	Panel *m = NULL;
	if (options::layout != SINGLE && left.bounds.contains(x,y)) m = &left;
	else if (options::layout != SINGLE && right.bounds.contains(x,y)) m = &right;
	else if (options::layout == SINGLE && active->bounds.contains(x,y)) m = active;
	if (m)
	{
		active = m;
		while (dy < 0) { m->move_selection(REQ_SCROLL_UP);   ++dy; }
		while (dy > 0) { m->move_selection(REQ_SCROLL_DOWN); --dy; }
		redraw(2);
		return true;
	}

	const int W = COLS, H = LINES;
	const int total_time = curr_tags ? curr_tags->time : 0;

	// time bar --> seek
	if (y == H-1 && x >= 1 && x <= W-2 && W > 8)
	{
		if (total_time <= 0 || options::TimeBarLine.empty()) return false;
		client.handle_command(dy < 0 ? KEY_CMD_SEEK_BACKWARD : KEY_CMD_SEEK_FORWARD);
		return true;
	}

	return false;
}

bool Interface::handle_drag(int x, int y, int seq)
{
	if (options::layout == SINGLE) return false;

	if (seq < 0)
	{
		drag0 = -1;

		if (y == LINES-1)
		{
			if (!handle_click(x, y, false)) return false;
			dragTime = true;
			drag0 = x;
			return true;
		}
		else
		{
			dragTime = false;
			if (!(left.bounds+right.bounds).contains(x,y)) return false;
			if (options::layout == HSPLIT)
			{
				if (x != left.bounds.x1() && x != right.bounds.x-1) return false;
				drag0 = x;
				options::hsplit = std::make_pair(right.bounds.w, left.bounds.w);
			}
			else
			{
				if (y != left.bounds.y-1) return false;
				drag0 = y;
				options::vsplit = std::make_pair(right.bounds.h, left.bounds.h);
			}
			redraw(2); // draw frame in selected color
			return true;
		}
	}

	if (drag0 < 0) return false;
	
	if (dragTime)
	{
		x = std::min(COLS-2, std::max(1, x));
		if (seq == 0 && x != drag0) // don't fire on mouse-up
		{
			handle_click(x, LINES-1, false);
			drag0 = x;
		}
	}
	else
	{
		int p = (options::layout == HSPLIT ? x : y);
		auto &r = CURR_RATIO;
		int s = (options::layout == HSPLIT ? +1 : -1);

		if (p != drag0)
		{
			redraw(2);
			r.first  -= s*(p-drag0);
			r.second += s*(p-drag0);
			if (r.first  < 0) { r.second -= r.first;  r.first  = 0; }
			if (r.second < 0) { r.first  -= r.second; r.second = 0; }
			drag0 = p;
		}

		if (seq > 0) redraw(2); // put frame back to normal color
	}

	if (seq > 0) drag0 = -1;

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

		key_cmd cmd = get_key_cmd (CON_ENTRY, c, f);
		switch (f)
		{
			case KEY_LEFT:  --cursor; return;
			case KEY_RIGHT: ++cursor; return;
			case KEY_HOME: cursor = 0; return;
			case KEY_END: cursor = strwidth(response); return;

			case KEY_BACKSPACE: if (cursor) strdel(response, --cursor);  return;
			case KEY_DC: if (cursor) strdel(response, cursor); return;
		}
		switch (cmd)
		{
			case KEY_CMD_CANCEL: prompting = false; return;
			case KEY_CMD_HISTORY_UP: /*TODO*/ return;
			case KEY_CMD_HISTORY_DOWN: /*TODO*/ return;
			case KEY_CMD_DELETE_START: strdel(response, 0, cursor); cursor = 0; return;
			case KEY_CMD_DELETE_END: strdel(response, cursor, strwidth(response)); return;
		}
	}
	else if (f == KEY_MOUSE)
	{
		MEVENT ev;
		while (getmouse(&ev) == OK)
		{
			// if dragging does not work, the following might help:
			// $ export TERM=xterm-1002

			//#define MOUSE_DEBUG(T) status(format("MOUSE %s (%d,%d) %d", T, ev.x, ev.y, (int)ev.bstate))
			#define MOUSE_DEBUG(...) 

			if ((ev.bstate & BUTTON1_RELEASED))
			{
				MOUSE_DEBUG("UP");
				//mousemask(MOUSEMASK, NULL);
				handle_drag(ev.x, ev.y, 1);
			}
			else if ((ev.bstate & REPORT_MOUSE_POSITION))
			{
				MOUSE_DEBUG(drag0 >= 0 ? "DRAG" : "MOVE");
				handle_drag(ev.x, ev.y, 0);
			}
			else if ((ev.bstate & BUTTON1_PRESSED))
			{
				MOUSE_DEBUG("DOWN");
				if (handle_drag(ev.x, ev.y, -1))
					;//mousemask(BUTTON1_RELEASED /*| REPORT_MOUSE_POSITION*/, NULL);
					 // ^-- did not work at all
			}
			else if ((ev.bstate & (BUTTON1_CLICKED|BUTTON1_DOUBLE_CLICKED)))
			{
				MOUSE_DEBUG("CLICK");
				handle_click(ev.x, ev.y, ev.bstate & BUTTON1_DOUBLE_CLICKED);
			}
			else if ((ev.bstate & (BUTTON4_PRESSED|BUTTON5_PRESSED)))
			{
				bool up = ev.bstate & BUTTON4_PRESSED;
				MOUSE_DEBUG(up ? "SCROLL UP" : "SCROLL DOWN");
				handle_scroll(ev.x, ev.y, up ? -1 : 1);
			}
			else
			{
				MOUSE_DEBUG("GARBAGE");
			}
		}
	}
	else
	{
		//status(format("KEY %d %d", (int)c, (int)f));
		auto cmd = get_key_cmd (CON_MENU, c, f);
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
			client.handle_command(cmd);
			break;
		}
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

	const auto frame_color = (drag0 > 0 && !dragTime) ? CLR_TIME_BAR_FILL : CLR_FRAME;

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
	
	str mhome = options::MusicDir; if (!mhome.empty()) mhome += '/'; if (mhome.length() < 2) mhome.clear();
	str uhome = options::Home;     if (!uhome.empty()) uhome += '/'; if (uhome.length() < 2) uhome.clear();

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
		if (options::layout != SINGLE) other->draw(false);
		active->draw(true);

		if (options::layout != SINGLE || active == &left)
		{
			str s = client.cwd; normalize_path(s);

			if (!mhome.empty() && has_prefix(s, mhome, false))
			{
				s = s.substr(mhome.length());
			}
			else if (!uhome.empty() && has_prefix(s, uhome, false))
			{
				assert(uhome.length() >= 3);
				s = s.substr(uhome.length()-2);
				s[0] = '~'; s[1] = '/';
			}
			else if (s+"/" == uhome) s = "~";
			sanitize(s);
			if (options::FileNamesIconv) s = files_iconv_str (s);
			if (options::UseRCCForFilesystem) s = rcc_reencode(s);
			win.frame(frame_color, r1, s, options::layout == VSPLIT ? 14 : 0, false);
		}
		if (options::layout != SINGLE || active == &right)
			win.frame(frame_color, r2, client.synced ? "Playlist" : "Playlist (local)", 0, false);

		// bottom frame
		win.color(frame_color);
		win.put(H-4, 0, win.ltee);
		win.hl(W-2);
		win.put(H-4, W-1, win.rtee);
		if (options::layout == HSPLIT)
		{
			win.put(H-4, left.bounds.x1(), win.lrcorn);
			win.put(win.llcorn);
		}
		else if (options::layout == VSPLIT)
		{
			int y = r2.y1()-1;
			win.put(y,   0, win.ltee);
			win.put(y, W-1, win.rtee);
		}

		// sides and corners
		win.put(H-3, 0,   win.vert);
		win.put(H-2, 0,   win.vert);
		win.put(H-3, W-1, win.vert);
		win.put(H-2, W-1, win.vert);
		win.put(H-1, 0,   win.llcorn);
		win.put(H-1, W-1, win.lrcorn);

		// playlist total times
		if (left.bounds.w >= 30 && (options::layout != SINGLE || active == &left))
		{
			int x1 = left.bounds.x1()-1, x0 = x1 - Window::TOTAL_TIME_WIDTH - 1;
			int y = left.bounds.y1();
			
			win.color(CLR_PLIST_TIME);
			win.moveto(y, x0+1);
			win.total_time(left.items.total_time());
	
			win.color(frame_color);
			win.put(y, x0, '[');
			win.put(y, x1, ']');
		}
		if (right.bounds.w >= 30 && (options::layout != SINGLE || active == &right))
		{
			int x1 = right.bounds.x1()-1, x0 = x1 - Window::TOTAL_TIME_WIDTH - 1;
			int y = right.bounds.y1();

			win.color(CLR_PLIST_TIME);
			win.moveto(y, x0+1);
			win.total_time(right.items.total_time());

			win.color(frame_color);
			win.put(y, x0, '[');
			win.put(y, x1, ']');
		}
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

	// current song: play/pause state
	win.color(CLR_BACKGROUND);
	win.moveto(H-3, 1); win.clear(W-2);
	win.color(CLR_STATE);
	win.moveto(H-3, 1);
	switch (state) {
		case STATE_PLAY:  win.put_ascii(" > "); break;
		case STATE_STOP:  win.put_ascii("[] "); break;
		case STATE_PAUSE: win.put_ascii("|| "); break;
		default: win.put_ascii("BUG"); break;
	}

	// current song title or message
	if (!messages.empty())
	{
		str msg = messages.front();
		win.color(has_prefix(msg, "ERROR", true) ? CLR_ERROR : CLR_MESSAGE);
		win.field(msg, W-5);
	}
	else
	{
		win.color(CLR_TITLE);

		str s = curr_file;
		if (!mhome.empty() && has_prefix(s, mhome, false))
		{
			s = s.substr(mhome.length());
		}
		else if (!uhome.empty() && has_prefix(s, uhome, false))
		{
			assert(uhome.length() >= 3);
			s = s.substr(uhome.length()-2);
			s[0] = '~'; s[1] = '/';
		}
		sanitize(s);
		if (options::FileNamesIconv) s = files_iconv_str (s);
		if (options::UseRCCForFilesystem) s = rcc_reencode(s);

		win.field(s, W-6, 'l');
	}

	// time bar
	str c1 = options::TimeBarLine, c2 = options::TimeBarSpace;
	if (total_time <= 0 || c1.empty())
	{
		win.moveto(H-1, 1);
		win.color(frame_color);
		win.hl(W-2);
	}
	else
	{
		int l1 = std::max(0, std::min(W-2, (W-2)*curr_time/total_time));
		if (c1.empty()) c1 = win.horiz;
		if (c2.empty()) c2 = c1;
		win.moveto(H-1, 1);
		win.color(CLR_TIME_BAR_FILL);
		for (int x = 0; x < l1; ++x) win.put(c1);
		win.color(CLR_TIME_BAR_EMPTY);
		for (int x = l1; x < W-2; ++x) win.put(c2);
	}

	// prompt (must be last so the cursor stays in the right place) or info
	if (prompting)
	{
		win.moveto(H-2, 1);
		win.color(CLR_ENTRY_TITLE);
		win.put_ascii(prompt_str);
		int x0 = 1+strwidth(prompt_str)+1;
		if (prompt_str.back() != '?' && prompt_str.back() != ':') { win.put(':'); ++x0; }
		win.put(' ');
		
		int w = W-1-x0;
		win.color(CLR_ENTRY);
		int n = strwidth(response);
		cursor = CLAMP(0, cursor, n);
		int nn = n + (cursor==n);
		if (w >= nn || hscroll < 0) hscroll = 0;
		if (cursor-hscroll < 5) hscroll = std::max(0, cursor-5);
		if (hscroll+w-cursor < 5) hscroll = std::max(0, 5+cursor-w);
		if (hscroll > 0 && n-hscroll+(cursor==n) > w)
			hscroll = std::max(0, n-w+(cursor==n));

		if (hscroll > 0)
		{
			win.put_ascii("...");
			win.field(xstrtail(response, n-3), w-3);
		}
		else
		{
			win.field(response, w);
		}
		
		// no more drawing after this!
		win.moveto(H-2, x0 + cursor-hscroll);
		curs_set(2);
	}
	else
	{
		win.moveto(H-2, 1);
		win.color(CLR_BACKGROUND);
		win.clear(W-2);

		curs_set(0);
		int w = W-2, x = 1;

		// toggles
		if (w >= w_toggles)
		{
			win.moveto(H-2, x+w-w_toggles);
			#define SW(x, v) win.color((v) ? CLR_INFO_ENABLED : CLR_INFO_DISABLED);\
				win.put_ascii("[" #x "]")
			SW(STEREO, channels==2); win.put(' ');
			SW(NET, is_url(curr_file.c_str())); win.put(' ');
			SW(SHUFFLE, options::Shuffle); win.put(' ');
			SW(REPEAT, options::Repeat); win.put(' ');
			SW(NEXT, options::AutoNext);
			#undef SW
			w -= w_toggles + 1;
		}

		// time for current song
		constexpr int TW = Window::TIME_WIDTH;
		win.color(CLR_TIME_CURRENT);
		if (w >= 5)
		{
			win.moveto(H-2, x);
			win.time(curr_time);
			x += TW+1; // time + one space
			w -= TW+1;
		}
		if (w >= 13)
		{
			win.moveto(H-2, x); win.time(total_time - curr_time);
			win.moveto(H-2, x+TW+2); win.time(total_time);
			win.color(CLR_TIME_TOTAL_FRAMES);
			win.put(H-2, x+TW+1,   '[');
			win.put(H-2, x+2*TW+2, ']');
			x += 2*TW+4; // printed stuff + one space
			w -= 2*TW+4;
		}
		++x; --w; // two spaces to whatever comes next

		// rate, bitrate, volume
		if (w >= 15)
		{
			win.moveto(H-2, x);
			win.color(CLR_LEGEND);
			win.put_ascii("   kHz     kbps");		

			win.color(CLR_SOUND_PARAMS);
			win.moveto(H-2, x);
			if (rate >= 0) win.put_ascii(format("%3d", rate)); else win.put_ascii("   ");
			win.moveto(H-2, x+7);
			if (bitrate >= 0) win.put_ascii(format("%4d", std::min(bitrate, 9999))); else win.put_ascii("    ");
			x += 15+2;
			w -= 15+2;
		}

		if (options::ShowMixer)
		{
			str ms = format("%s: %02d%%", mixer_name.c_str(), mixer_value);
			if (w >= ms.length())
			{
				win.color(CLR_SOUND_PARAMS);
				win.moveto(H-2, x);
				win.put(ms);
				//x += ms.length()+2; w -= ms.length()+2
			}
		}
	}

	need_redraw = 0;
	win.flush();
}
