#ifndef UTF8_H
#define UTF8_H

# include <ncurses.h>

#include <stdarg.h>
#ifdef HAVE_ICONV
# include <iconv.h>
#endif

/* parameter passed to wcswidth() as a maximum width */
#define WIDTH_MAX	2048

void utf8_init ();
void utf8_cleanup ();
int xwaddstr (WINDOW *win, const char *str);
int xwaddnstr (WINDOW *win, const char *str, const int n);
int xmvwaddstr (WINDOW *win, const int y, const int x, const char *str);
int xmvwaddnstr (WINDOW *win, const int y, const int x, const char *str,
		const int n);
int xwprintw (WINDOW *win, const char *fmt, ...) ATTR_PRINTF(2, 3);
size_t strwidth (const char *s);
char *xstrtail (const char *str, const int len);
char *iconv_str (const iconv_t desc, const char *str);
char *files_iconv_str (const char *str);
char *xterm_iconv_str (const char *str);

#endif
