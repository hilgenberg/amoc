#pragma once

#include "Rect.h"
# include <ncurses.h>
# include <iconv.h>

void utf8_init ();
void utf8_cleanup ();

int xwaddstr (WINDOW *win, const str &s);
int xmvwaddstr (WINDOW *win, const int y, const int x, const str &s);

size_t strwidth (const str &s);
void strdel(str &s, int i, int n = 1);
int  strins(str &s, int i, wchar_t c); // returns width of c

void xwprintfield(WINDOW *win, const str &s, int field_width, char ellipsis = 'r');

str xstrtail (const str &s, int len);

str iconv_str (const iconv_t desc, const str &input);
str files_iconv_str (const str &s);
