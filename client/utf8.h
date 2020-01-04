#ifndef UTF8_H
#define UTF8_H

# include <ncurses.h>

#include <stdarg.h>
#ifdef HAVE_ICONV
# include <iconv.h>
#endif

#include "Rect.h"

void utf8_init ();
void utf8_cleanup ();
inline int xwaddstr (WINDOW *win, const str &s) { return xwaddstr(win, s.c_str()); };
int xwaddstr (WINDOW *win, const char *str);
int xwaddnstr (WINDOW *win, const char *str, const int n);
int xmvwaddstr (WINDOW *win, const int y, const int x, const char *str);
inline int xmvwaddstr (WINDOW *win, const int y, const int x, const str &s) { return xmvwaddstr (win, y, x, s.c_str()); }
int xmvwaddnstr (WINDOW *win, const int y, const int x, const char *str, const int n);
int xwprintw (WINDOW *win, const char *fmt, ...);

size_t strwidth (const char *s);
size_t strwidth (const str &s);
void strdel(str &s, int i, int n = 1);
int  strins(str &s, int i, wchar_t c); // returns width of c

void xwprintfield(WINDOW *win, const str &s, int field_width, char ellipsis = 'r');
void clear_area (WINDOW *win, const Rect &r);

str xstrtail (const str &s, int len);
char *xstrtail (const char *str, const int len);
char *iconv_str (const iconv_t desc, const char *str);
char *files_iconv_str (const char *str);
char *xterm_iconv_str (const char *str);
str files_iconv_str (const str &s);

#endif
