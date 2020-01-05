#include <locale.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/select.h>
#include <wctype.h>
#include <wchar.h>

#include "interface.h"
#include "client.h"
#include "utf8.h"
#include "menu.h"
#include "themes.h"
#include "keys.h"
#include "../playlist.h"
#include "../protocol.h"
#include "../rcc.h"
#include "../server/input/decoder.h"
#include "../server/output/softmixer.h"
#include "../server/ratings.h"

#define STATUS_WIDTH 26

/* Chars used to make lines (for borders etc.). */
static struct
{
	chtype vert;	/* vertical */
	chtype horiz;	/* horizontal */
	chtype ulcorn;	/* upper left corner */
	chtype urcorn;	/* upper right corner */
	chtype llcorn;	/* lower left corner */
	chtype lrcorn;	/* lower right corner */
	chtype rtee;	/* right tee */
	chtype ltee;	/* left tee */
	chtype btee;	/* right tee */
	chtype ttee;	/* left tee */
} lines;

static void init_lines ()
{
	if (options::ASCIILines) {
		lines.vert = '|';
		lines.horiz = '-';
		lines.ulcorn = '+';
		lines.urcorn = '+';
		lines.llcorn = '+';
		lines.lrcorn = '+';
		lines.rtee = '|';
		lines.ltee = '|';
		lines.btee = '+';
		lines.ttee = '+';
	} else {
		lines.vert = ACS_VLINE;
		lines.horiz = ACS_HLINE;
		lines.ulcorn = ACS_ULCORNER;
		lines.urcorn = ACS_URCORNER;
		lines.llcorn = ACS_LLCORNER;
		lines.lrcorn = ACS_LRCORNER;
		lines.rtee = ACS_RTEE;
		lines.ltee = ACS_LTEE;
		lines.btee = ACS_BTEE;
		lines.ttee = ACS_TTEE;
	}
}

static void draw_frame(WINDOW *win, const Rect &r, const str &title, bool draw_bottom = true)
{
	if (r.w < 2 || r.h < 2) return;

	wattrset (win, get_color(CLR_FRAME));
	wmove (win, r.y, r.x);
	waddch (win, lines.ulcorn);
	whline (win, lines.horiz, r.w - 2);
	wmove (win, r.y, r.x+r.w-1);
	waddch (win, lines.urcorn);
	wmove (win, r.y + 1, r.x);
	wvline (win, lines.vert, r.h - 2);
	wmove (win, r.y + 1, r.x + r.w - 1);
	wvline (win, lines.vert, r.h - 2);
	if (draw_bottom) {
		wmove (win, r.y + r.h - 1, r.x);
		waddch (win, lines.llcorn);
		whline (win, lines.horiz, r.w - 2);
		wmove (win, r.y + r.h - 1, r.x + r.w - 1);
		waddch (win, lines.lrcorn);
	}

	if (!title.empty() && r.w > 4)
	{
		int tw = std::min((int)strwidth(title), r.w-4);
		wmove (win, r.y, r.x+(r.w-tw)/2-1);
		waddch(win, lines.rtee);
		wattrset (win, get_color(CLR_WIN_TITLE));
		xwprintfield(win, title, tw);
		wattrset (win, get_color(CLR_FRAME));
		waddch (win, lines.ltee);
	}
}

static str total_time_str(int sec)
{
	if (sec <= 0) return "-----:--:--";
	int h = sec / 3600;
	int m = (sec / 60) % 60;
	int s = sec % 60;
	if (h <= 99999) return format("%05d:%02d:%02d", h, m, s);
	return " very:lo:ng";
}
static str time_str(int sec)
{
	if (sec <= 0) return "--:--";
	int m = sec / 60;
	int s = sec % 60;
	if (m <= 99) return format("%02d:%02d", m, s);
	int h = m / 60; m %= 60;
	if (h <= 99) return format("%02dh%02d", h, m);
	int d = h/24; h %= 24;
	if (d <= 99) return format("%02dD%02d", d, h);
	int y = d/365; d %= 365; // um, yeah, really useful ;-)
	if (y <= 99) return format("%02dY%02d", d, h);
	return "??:??";

}

str Interface::cwd() const { return client.cwd; }

Interface::Interface(Client &client, plist &pl1, plist &pl2)
: client(client)
, win(NULL)
, left(this, pl1)
, right(this, pl2)
, active_menu(1)
, layout(HSPLIT)
, prompting(false)
, need_redraw(2)
, bitrate(-1), avg_bitrate(-1), rate(-1)
, curr_time(0), total_time(0), channels(0)
, state(STATE_STOP), mixer_value(-1)
, message_display_start(0)
{
	menus[0] = &left; menus[1] = &right;

	if (!getenv ("ESCDELAY"))
	#ifdef HAVE_SET_ESCDELAY
		set_escdelay (25);
	#else
		setenv ("ESCDELAY", "25", 0);
	#endif

	utf8_init ();
	if (!initscr ()) fatal ("Can't initialize terminal!");
	cbreak();
	noecho();
	curs_set(0);
	use_default_colors();

	start_color ();
	theme_init ();
	init_lines ();

	wnoutrefresh (win);
	doupdate ();

	win = newwin(LINES, COLS, 0, 0);
	wbkgd (win, get_color(CLR_BACKGROUND));
	nodelay (win, TRUE);
	keypad (win, TRUE);
}

Interface::~Interface()
{
	if (win) delwin(win);

	utf8_cleanup ();
	/* endwin() sometimes fails on X-terminals when we get SIGCHLD
	 * at this moment.  Double invocation seems to solve this. */
	if (endwin () == ERR && endwin () == ERR)
		logit ("endwin() failed!");
	/* Make sure that the next line after we exit will be clear. */
	printf ("\n");
	fflush (stdout);
}

void Interface::cycle_layouts()
{
	++(int&)layout;
	(int&)layout %= 3;
	redraw(2);
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
	if (menus[active_menu]->items.empty())
		if (!menus[1-active_menu]->items.empty()) active_menu = 1-active_menu;
	auto &am = *menus[active_menu];
	if (am.sel == -1 && am.mark == -1 && !am.items.empty())
		am.sel = 0;

	const int W = COLS, H = LINES;
	if (W < 30 || H < 7)
	{
		const char *msg = "...TERMINAL TOO SMALL...";
		werase (win);
		wbkgd (win, get_color(CLR_BACKGROUND));
		wmove (win, 0, 0);
		wattrset (win, get_color(CLR_MESSAGE));
		xmvwaddstr (win, LINES/2, COLS/2 - sizeof(msg)/2, msg);
		need_redraw = 0;
		return;
	}

	if (state == STATE_STOP)
	{
		curr_file.clear();
		curr_tags.reset(nullptr);
		bitrate = avg_bitrate = rate = 0;
		curr_time = total_time = 0;
		channels = 0;
	}

	if (need_redraw > 1) // tags or sizes changed?
	{
		werase (win);
		wbkgd (win, get_color(CLR_BACKGROUND));
		int m; Rect r1, r2; // for left+right = dir+plist
		switch (layout)
		{
			case HSPLIT:
				m = W/2;
				r1.set(0, 0, m, H-3);
				r2.set(m, 0, W-m, H-3);
				break;
			case VSPLIT:
				m = (H-3)/2;
				r2.set(0, 0, W, m);
				r1.set(0, m, W, H-3-m);
				break;
			case SINGLE:
				r1.set(0, 0, W, H-3);
				r2 = r1;
				break;
		}
		left.bounds = r1.inset(1);
		right.bounds = r2.inset(1);

		left.mark_path(curr_file);
		right.mark_path(curr_file);

		if (layout != SINGLE) menus[1-active_menu]->draw(false);
		menus[active_menu]->draw(true);
		str &curr_dir = client.cwd;
		if (layout != SINGLE || active_menu==0) draw_frame(win, r1, options::FileNamesIconv ? files_iconv_str(curr_dir) : curr_dir, false);
		if (layout != SINGLE || active_menu==1) draw_frame(win, r2, client.synced ? "Playlist" : "Playlist (local)", layout==VSPLIT);

		// bottom frame and playlist total time frame(s)
		wattrset (win, get_color(CLR_FRAME));
		mvwaddch (win, H-4, 0, lines.ltee);
		whline (win, lines.horiz, W-2);
		mvwaddch (win, H-4, W-1, lines.rtee);
		if (layout == HSPLIT)
		{
			int x = r1.x1();
			mvwaddch (win, H-4, x - 14, '[');
			mvwaddch (win, H-4, x - 2, ']');
			waddch(win, lines.lrcorn);
			waddch(win, lines.llcorn);
		}
		else if (layout == VSPLIT)
		{
			int y = r2.y1()-1;
			mvwaddch (win, y, W - 14, '[');
			mvwaddch (win, y, W - 2, ']');

		}
		mvwaddch (win, H-4, W - 14, '[');
		mvwaddch (win, H-4, W - 2, ']');

		// sides and corners
		mvwaddch (win, H-3, 0,   lines.vert);
		mvwaddch (win, H-2, 0,   lines.vert);
		mvwaddch (win, H-3, W-1, lines.vert);
		mvwaddch (win, H-2, W-1, lines.vert);
		mvwaddch (win, H-1, 0,   lines.llcorn);
		mvwaddch (win, H-1, W-1, lines.lrcorn);

		/* status line frame */
		mvwaddch (win, H-4, 5, lines.rtee);
		mvwaddch (win, H-4, 5 + STATUS_WIDTH + 1, lines.ltee);

		/* total time frames */
		wattrset (win, get_color(CLR_TIME_TOTAL_FRAMES));
		mvwaddch (win, H-2, 13, '[');
		mvwaddch (win, H-2, 19, ']');

		/* rate and bitrate units */
		wmove (win, H-2, 25);
		wattrset (win, get_color(CLR_LEGEND));
		xwaddstr (win, "kHz	 kbps");		

		// playlist total times
		wattrset (win, get_color(CLR_PLIST_TIME));
		if (layout != SINGLE || active_menu == 0)
		{
			str s = total_time_str(left.items.total_time());
			wmove(win, left.bounds.y1(), left.bounds.x1()-s.length()-1);
			xwaddstr (win, s);
		}
		if (layout != SINGLE || active_menu == 1)
		{
			str s = total_time_str(right.items.total_time());
			wmove(win, right.bounds.y1(), right.bounds.x1()-s.length()-1);
			xwaddstr (win, s);
		}
	}

	// time bar
	int l1 = total_time <= 0 ? 0 : (W-2)*curr_time/total_time;
	l1 = std::min(0, std::max(W-2, l1));
	str c1 = options::TimeBarLine; if (c1.empty()) c1 = lines.horiz;
	str c2 = options::TimeBarSpace; if (c2.empty()) c2 = lines.horiz;
	wmove (win, H-1, 1);
	wattrset(win, get_color(CLR_TIME_BAR_FILL));
	for (int x = 0; x < l1; ++x) xwaddstr (win, c1);
	wattrset(win, get_color(CLR_TIME_BAR_EMPTY));
	for (int x = l1; x < W-2; ++x) xwaddstr (win, c2);

	// state
	wattrset (win, get_color(CLR_STATE));
	wmove(win, H-3, 1);
	switch (state) {
		case STATE_PLAY:  xwaddstr(win, " > "); break;
		case STATE_STOP:  xwaddstr(win, "[] "); break;
		case STATE_PAUSE: xwaddstr(win, "|| "); break;
		default: xwaddstr(win, "??"); break;
	}

	// current song title or message
	if (!messages.empty())
	{
		str msg = messages.front();
		wattrset (win, get_color(has_prefix(msg, "ERROR", true) ? CLR_ERROR : CLR_MESSAGE));
		xwprintfield(win, msg, W-5);
	}
	else
	{
		wattrset (win, get_color (CLR_TITLE));
		xwprintfield(win, curr_file, W-5); // TODO
	}

	// status message
	wattrset (win, get_color(CLR_FRAME));
	if (status_msg.empty())
	{
		wmove (win, H-4, 5);
		whline (win, lines.horiz, STATUS_WIDTH+2);
	}
	else
	{
		mvwaddch (win, H-4, 5, lines.rtee);
		mvwaddch (win, H-4, 5 + STATUS_WIDTH + 1, lines.ltee);
		wattrset (win, get_color(CLR_STATUS));
		wmove (win, H-4, 6);
		xwprintfield(win, status_msg, STATUS_WIDTH, 'c');
	}

	// prompt or info
	if (prompting)
	{
		wmove (win, H-2, 1);
		wattrset (win, get_color(CLR_ENTRY_TITLE));
		xwaddstr(win, prompt_str);
		int x0 = 1+strwidth(prompt_str)+1;
		if (prompt_str.back() != '?' && prompt_str.back() != ':') { waddch(win, ':'); ++x0; }
		waddch(win, ' ');
		
		int w = W-1-x0;
		wattrset (win, get_color(CLR_ENTRY));
		int n = strwidth(response);
		cursor = CLAMP(0, cursor, n);
		if (cursor-hscroll < 5) hscroll = std::max(0, cursor-5);
		if (hscroll+w-cursor < 5) hscroll = std::max(0, 5+cursor-w);
		if (hscroll > 0 && n-hscroll+(cursor==n) > w)
			hscroll = std::max(0, n-w+(cursor==n));

		if (hscroll > 0)
		{
			waddstr(win, "...");
			xwprintfield(win, xstrtail(response, n-3), w-3);
		}
		else
		{
			xwprintfield(win, response, w);
		}
		
		// no more drawing after this!
		wmove (win, H-2, x0 + cursor-hscroll);
		curs_set(2);
	}
	else
	{
		curs_set(0);

		// time for current song
		wattrset (win, get_color(CLR_TIME_CURRENT));
		xmvwaddstr(win, H-2, 1, time_str(curr_time));
		xmvwaddstr(win, H-2, 7, time_str(total_time - curr_time));
		xmvwaddstr(win, H-2, 14, time_str(total_time));

		// rate, bitrate, volume
		wattrset (win, get_color(CLR_SOUND_PARAMS));
		wmove (win, H-2, 22);
		if (rate >= 0) xwprintw (win, "%3d", rate); else xwaddstr (win, "   ");
		wmove (win, H-2, 29);
		if (bitrate >= 0) xwprintw (win, "%4d", std::min(bitrate, 9999)); else xwaddstr(win, "    ");
		wmove(win, H-2, 80); // TODO
		xwprintw(win, "%s: %02d", mixer_name.c_str(), CLAMP(0, mixer_value, 100)); 

		// toggles
		wmove (win, H-2, 38);
		#define SW(x, v) wattrset(win, get_color((v) ? CLR_INFO_ENABLED : CLR_INFO_DISABLED));\
			xwaddstr(win, "[" #x "]"); waddch(win, ' ')
		SW(STEREO, channels==2);
		SW(NET, is_url(curr_file.c_str()));
		SW(SHUFFLE, options::Shuffle);
		SW(REPEAT, options::Repeat);
		SW(NEXT, options::AutoNext);
		#undef SW
	}

	need_redraw = 0;
	wrefresh(win);
}

void Interface::resize ()
{
	endwin();
	refresh();
	keypad (win, TRUE);
	wresize (win, LINES, COLS);
	werase (win);
	redraw(2);
}

void Interface::handle_input()
{
	wchar_t c = 0; // regular key
	int     f = 0; // function key

	wint_t ch = wgetch(win);
	if (ch == (wint_t)ERR) interface_fatal ("wgetch() failed!");

	if (ch < 32 && ch != '\n' && ch != '\t' && ch != KEY_ESCAPE)
	{
		/* Unprintable, generally control sequences */
		f = ch;
	}
	else if (ch == 0x7f)
	{
		/* Workaround for backspace on many terminals */
		f = KEY_BACKSPACE;
	}
	else if (ch < 255)
	{
		/* Regular char */
		ungetch (ch);
		if (wget_wch(win, &ch) == ERR) interface_fatal ("wget_wch() failed!");

		/* Recognize meta sequences */
		if (ch == KEY_ESCAPE) {
			int meta = wgetch (win);
			if (meta != ERR) ch = meta | META_KEY_FLAG;
			f = ch;
		}
		else c = ch;
	}
	else f = ch;

	auto &panel = *menus[active_menu];

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
	else
	{
		auto cmd = get_key_cmd (CON_MENU, c, f);
		switch (cmd)
		{
		case KEY_CMD_MENU_DOWN:  panel.move(REQ_DOWN);   redraw(2); break;
		case KEY_CMD_MENU_UP:    panel.move(REQ_UP);     redraw(2); break;
		case KEY_CMD_MENU_NPAGE: panel.move(REQ_PGDOWN); redraw(2); break;
		case KEY_CMD_MENU_PPAGE: panel.move(REQ_PGUP);   redraw(2); break;
		case KEY_CMD_MENU_FIRST: panel.move(REQ_TOP);    redraw(2); break;
		case KEY_CMD_MENU_LAST:  panel.move(REQ_BOTTOM); redraw(2); break;
		case KEY_CMD_TOGGLE_MENU: active_menu = 1-active_menu; redraw(2); break;
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
				prompt("SAVE PLAYLIST", NULL, [this](){
					if (response.empty()) return;
					if (file_exists(response.c_str())) {
						prompt("File exists, overwrite (y/n)?", NULL, [this](){
							if (response == "y")
							{
								status("Saving the playlist...");
								client.playlist.save(response.c_str());
								status("");
							}
							else
								status("Aborted.");
						});
					}
					else
					{
						status("Saving the playlist...");
						client.playlist.save(response.c_str());
						status("");
					}
				});
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

void Interface::move_down()
{
	auto &panel = *menus[active_menu];
	panel.move(REQ_DOWN);
	redraw(2);
}

bool Interface::sel_item(const plist_item *what, int k)
{
	auto *panel = menus[k==0 || k==1 ? k : active_menu];
	if (!what) { panel->sel = -1; return true; }

	auto &plist = panel->items;
	int i = 0; const int n = plist.size();

	// match the item if possible
	for (i = 0; i < n; ++i)
	{
		if (what == plist.items[i].get())
		{
			panel->sel = i;
			return true;
		}
	}
	// otherwise try the path (take last matching item)
	for (i = n-1; i >= 0; --i)
	{
		if (what->path == plist.items[i]->path)
		{
			panel->sel = i;
			return true;
		}
	}
	return false;
}

void Interface::prompt(const str &prompt, stringlist *history, std::function<void(void)> cb)
{
	prompting = true;
	prompt_str = prompt;
	response.clear();
	cursor = hscroll = 0;
	callback = cb;
}


























#if 0
/* Add a char to the entry where the cursor is placed. */
static void entry_add_char (struct entry *e, const wchar_t c)
{
	size_t len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (len >= ARRAY_SIZE(e->text_ucs) - sizeof(wchar_t))
		return;

	memmove (e->text_ucs + e->cur_pos + 1,
			e->text_ucs + e->cur_pos,
			(len - e->cur_pos + 1) * sizeof(e->text_ucs[0]));
	e->text_ucs[e->cur_pos] = c;
	e->cur_pos++;

	if (e->cur_pos - e->display_from > e->width)
		e->display_from++;
}

/* Delete 'count' chars before the cursor. */
static void entry_del_chars (struct entry *e, int count)
{
	assert (e != NULL);
	assert (e->cur_pos > 0);

	int width = wcslen (e->text_ucs);
	if (e->cur_pos < count)
		count = e->cur_pos;

	memmove (e->text_ucs + e->cur_pos - count,
	         e->text_ucs + e->cur_pos,
	         (width - e->cur_pos) * sizeof (e->text_ucs[0]));
	width -= count;
	e->text_ucs[width] = L'\0';
	e->cur_pos -= count;

	if (e->cur_pos < e->display_from)
		e->display_from = e->cur_pos;

	/* Can we show more after deleting the chars? */
	if (e->display_from > 0 && width - e->display_from < e->width)
		e->display_from = width - e->width;
	if (e->display_from < 0)
		e->display_from = 0;
}

/* Delete the char before the cursor. */
static void entry_back_space (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0)
		entry_del_chars (e, 1);
}

/* Delete the char under the cursor. */
static void entry_del_char (struct entry *e)
{
	int len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (e->cur_pos < len) {
		e->cur_pos += 1;
		entry_del_chars (e, 1);
	}
}
/* Delete the chars from cursor to start of line. */
static void entry_del_to_start (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0)
		entry_del_chars (e, e->cur_pos);
}

/* Delete the chars from cursor to end of line. */
static void entry_del_to_end (struct entry *e)
{
	int len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (e->cur_pos < len) {
		int count;

		count = len - e->cur_pos;
		e->cur_pos = len;
		entry_del_chars (e, count);
	}
}

static char *entry_get_text (const struct entry *e)
{
	char *text;
	int len;

	assert (e != NULL);

	len = wcstombs (NULL, e->text_ucs, -1) + 1;
	assert (len >= 1);
	text = (char *) xmalloc (sizeof (char) * len);
	wcstombs (text, e->text_ucs, len);

	return text;
}
#endif
