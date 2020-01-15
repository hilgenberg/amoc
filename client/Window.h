#pragma once
#include <ncurses.h>
#include <wctype.h>
#include <wchar.h>
#include "Rect.h"
#include "themes.h"
#include "utf8.h"

//---------------------------------------------------------------
// Window wraps NCurses' WINDOW pointer and adds UTF-8 support
// as well as some convenience methods (like drawing formatted
// times f.i.).
//---------------------------------------------------------------

class Window
{
public:
	Window();
	~Window();
	WINDOW *WIN() { return win; }

	void resize();
	void clear();
	void flush() { wrefresh(win); }

	void attr(int c) { wattrset(win, c); }
	void color(color_index c) { wattrset(win, get_color(c)); }
	
	void moveto(int y, int x) { wmove(win, y, x); }
	void put(int y, int x, chtype c) { mvwaddch (win, y, x, c); }

	void put(chtype c) { waddch (win, c); }
	void put_ascii(const str &s) { waddstr(win, s.c_str()); }
	void put_ascii(const char *s) { waddstr(win, s); }
	void put(const str &s) { xwaddstr(win, s); }
	void field(const str &s, int w, char fmt = 'r') { xwprintfield(win, s, w, fmt); }
	void spaces(int n) { while (n-- > 0) waddch(win, ' '); } // unlike clear, this moves the cursor
	
	void time(int sec); static constexpr int TIME_WIDTH = 5;
	void total_time(int sec); static constexpr int TOTAL_TIME_WIDTH = 11;

	void hl(int w) { whline(win, horiz, w); } // hline is #defined in curses
	void vl(int h) { wvline(win, vert,  h); } // same here
	void clear(int w) { whline(win, ' ', w); }

	void frame(const Rect &r, const str &title, int title_space = 0, bool draw_bottom = true);

	static chtype vert;	// vertical
	static chtype horiz;	// horizontal
	static chtype ulcorn;	// upper left corner
	static chtype urcorn;	// upper right corner
	static chtype llcorn;	// lower left corner
	static chtype lrcorn;	// lower right corner
	static chtype rtee;	// right tee: -|
	static chtype ltee;	// left tee:  |-
	static chtype btee;	// bottom tee
	static chtype ttee;	// top tee: T

private:
	WINDOW *win;
};


