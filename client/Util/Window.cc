#include "Window.h"

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

Window::Window() : win(NULL)
{
	if (!getenv ("ESCDELAY")) set_escdelay(25);
	if (!options::TERM.empty()) setenv ("TERM", options::TERM.c_str(), 1);

	init_lines();
	utf8_init ();
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

	utf8_cleanup ();
	
	/* endwin() sometimes fails on X-terminals when we get SIGCHLD
	 * at this moment.  Double invocation seems to solve this. */
	if (endwin () == ERR && endwin () == ERR) logit ("endwin() failed!");

	/* Make sure that the next line after we exit will be clear. */
	//printf ("\n");
	fflush (stdout);
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
		xwprintfield(win, title, tw);
		wattrset (win, get_color(CLR_FRAME));
		waddch (win, ltee);
	}
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
	int y = d/365; d %= 365; // um, yeah, really useful ;-)
	if (y <= 99) return format("%02dY%02d", d, h);
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
