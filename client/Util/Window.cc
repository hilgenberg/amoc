#include "Window.h"
#include "rcc.h"
#include <langinfo.h>

chtype Window::vert, Window::horiz, Window::ulcorn, Window::urcorn, Window::llcorn, 
       Window::lrcorn, Window::rtee, Window::ltee, Window::btee, Window::ttee;

static void init_lines()
{
	if (options::HideBorder) {
		Window::vert = Window::horiz = Window::ulcorn = Window::urcorn = 
		Window::llcorn = Window::lrcorn = Window::rtee = Window::ltee = 
		Window::btee = Window::ttee = ' ';
	} else if (options::ASCIILines) {
		Window::vert   = '|';
		Window::horiz  = '-';
		Window::ulcorn = '+';
		Window::urcorn = '+';
		Window::llcorn = '+';
		Window::lrcorn = '+';
		Window::rtee   = '<';
		Window::ltee   = '>';
		Window::btee   = '+';
		Window::ttee   = '+';
	} else {
		Window::vert   = ACS_VLINE;
		Window::horiz  = ACS_HLINE;
		Window::ulcorn = ACS_ULCORNER;
		Window::urcorn = ACS_URCORNER;
		Window::llcorn = ACS_LLCORNER;
		Window::lrcorn = ACS_LRCORNER;
		Window::rtee   = ACS_RTEE;
		Window::ltee   = ACS_LTEE;
		Window::btee   = ACS_BTEE;
		Window::ttee   = ACS_TTEE;
	}
}

static str iconv_str(const iconv_t desc, const str &input)
{
	if (input.empty() || desc == (iconv_t)-1) return input;

	iconv (desc, NULL, NULL, NULL, NULL); // reset state
	
	str ret;
	size_t n = input.size(), m = 0;
	const char *s = input.data(); // current input position
	char *t = NULL; // current output position

	while (n)
	{
		size_t min_buf = std::max((size_t)8, n);
		if (!t || m < min_buf)
		{
			size_t m0 = ret.size()-m;
			m = min_buf;
			ret.resize(m0 + m);
			t = (char*)ret.data() + m0;
		}
		if (iconv(desc, (char**)&s, &n, &t, &m) == (size_t)-1)
		{
			if (errno == EILSEQ || errno == EINVAL) {
				// invalid sequence at s: skip first byte and try again
				if (n) { ++s; --n; }
				if (m) { *t++ = '#'; --m; } else ret.push_back('#');
			}
			else if (errno != E2BIG) {
				assert(false);
				break;
			}
		}
	}

	ret.resize(ret.size() - m);
	return ret;
}

Window::Window()
: win(NULL), using_utf8(false)
, term_iconv_desc((iconv_t)(-1))
, files_iconv_desc((iconv_t)(-1))
{
	if (!getenv ("ESCDELAY")) set_escdelay(25);
	if (!options::TERM.empty()) setenv ("TERM", options::TERM.c_str(), 1);

	init_lines();
	rcc_init ();
	
	const char *terminal_charset = nl_langinfo(CODESET);
	if (!strcmp(terminal_charset, "UTF-8")) {
		logit ("Using UTF8 output");
		using_utf8 = true;
	}
	else if (terminal_charset)
	{
		logit ("Terminal character set: %s", terminal_charset);
		term_iconv_desc = iconv_open (terminal_charset, "UTF-8");
		if (term_iconv_desc == (iconv_t)(-1)) log_errno ("iconv_open() failed", errno);
	}
	if (options::FileNamesIconv)
		files_iconv_desc = iconv_open ("UTF-8", "");

	if (!initscr ()) fatal ("Can't initialize terminal!");
	cbreak();
	noecho();
	curs_set(0);
	use_default_colors();

	start_color ();
	theme_init ();
	init_lines ();

	win = newwin(LINES, COLS, 0, 0);
	wbkgd (win, get_color(CLR_BACKGROUND));
	nodelay (win, TRUE);
	keypad (win, TRUE);
}

void Window::resize()
{
	endwin();
	refresh();
	keypad (win, TRUE);
	wresize (win, LINES, COLS);
}

Window::~Window()
{
	if (win) delwin(win);

	if (term_iconv_desc != (iconv_t)-1 && iconv_close(term_iconv_desc) == -1)
		log_errno ("iconv_close() failed", errno);
	if (files_iconv_desc != (iconv_t)-1 && iconv_close(files_iconv_desc) == -1)
		log_errno ("iconv_close() failed", errno);

	/* endwin() sometimes fails on X-terminals when we get SIGCHLD
	 * at this moment.  Double invocation seems to solve this. */
	if (endwin () == ERR && endwin () == ERR) logit ("endwin() failed!");

	/* Make sure that the next line after we exit will be clear. */
	//printf ("\n");
	fflush (stdout);

	rcc_cleanup ();
}

void Window::clear()
{
	wbkgd (win, get_color(CLR_BACKGROUND));
	werase (win);
}

void Window::frame(const Rect &r, const str &title, int title_space, bool draw_bottom)
{
	if (r.w < 2 || r.h < 2) return;

	wattrset (win, get_color(CLR_FRAME));
	wmove (win, r.y, r.x);
	waddch (win, ulcorn);
	whline (win, horiz, r.w - 2);
	wmove (win, r.y, r.x+r.w-1);
	waddch (win, urcorn);
	wmove (win, r.y + 1, r.x);
	wvline (win, vert, r.h - 2);
	wmove (win, r.y + 1, r.x + r.w - 1);
	wvline (win, vert, r.h - 2);
	if (draw_bottom) {
		wmove (win, r.y + r.h - 1, r.x);
		waddch (win, llcorn);
		whline (win, horiz, r.w - 2);
		wmove (win, r.y + r.h - 1, r.x + r.w - 1);
		waddch (win, lrcorn);
	}

	if (!title.empty() && r.w > 4 + 2*title_space)
	{
		int tw = std::min((int)strwidth(title), r.w-4-2*title_space);
		wmove (win, r.y, r.x+(r.w-tw)/2-1);
		waddch(win, rtee);
		wattrset (win, get_color(CLR_WIN_TITLE));
		field(title, tw);
		wattrset (win, get_color(CLR_FRAME));
		waddch (win, ltee);
	}
}

void Window::sanitize_path(str &s)
{
	sanitize(s);
	if (options::FileNamesIconv) s = iconv_str(files_iconv_desc, s);
	if (options::UseRCCForFilesystem) s = rcc_reencode(s);
}

static str time_str(int sec)
{
	if (sec < 0) return "--:--";
	int m = sec / 60;
	int s = sec % 60;
	if (m <= 99) return format("%02d:%02d", m, s);
	int h = m / 60; m %= 60;
	if (h <= 99) return format("%02dh%02d", h, m);
	int d = h/24; h %= 24;
	if (d <= 99) return format("%02dD%02d", d, h);
	return "++:++";
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

void Window::time(int sec)
{
	put_ascii(time_str(sec));
}
void Window::total_time(int sec)
{
	put_ascii(total_time_str(sec));
}

void Window::put(const str &s)
{
	waddstr(win, using_utf8 ? s.c_str() : iconv_str(term_iconv_desc, s).c_str());
}

void Window::field(const str &s, int W, char fmt)
{
	int w = strwidth(s);
	if (w <= W)
	{
		if (fmt == 'R')
		{
			while (w++ < W) waddch (win, ' ');
			put(s);
		}
		else if (fmt == 'C')
		{
			int k = W-w;
			for (int i = 0; i < k/2; ++i) waddch (win, ' ');
			put(s);
			for (int i = k/2; i < k; ++i) waddch (win, ' ');
		}
		else
		{
			put(s);
			while (w++ < W) waddch (win, ' ');
		}
		return;
	}
	
	if (W < 3)
	{
		put(strhead(s, W));
		return;
	}

	switch (fmt)
	{
		case 'r':
		case 'C':
			put(strhead(s, W-3));
			put_ascii("...");
			break;
		case 'l':
		case 'R':
		{
			put_ascii("...");
			put(strtail(s, W-3));
			break;
		}
		case 'c':
		{
			int l = (W-3)/2;
			put(strhead(s, l));
			put_ascii("...");
			put(strtail(s, W-3-l));
			break;
		}
		default: assert(false);
	}
}
