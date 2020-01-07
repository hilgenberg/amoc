/*
 * MOC - music on console
 * Copyright (C) 2005,2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#define WIDTH_MAX 2048*1024

#ifdef HAVE_ICONV
# include <iconv.h>
#endif
#ifdef HAVE_NL_TYPES_H
# include <nl_types.h>
#endif
#ifdef HAVE_LANGINFO_H
# include <langinfo.h>
#endif

#include <ncurses.h>
#include <wchar.h>

#include "utf8.h"
#include "../rcc.h"
#include "themes.h"

static char *terminal_charset = NULL;
static int using_utf8 = 0;

static iconv_t iconv_desc = (iconv_t)(-1);
static iconv_t files_iconv_desc = (iconv_t)(-1);
static iconv_t xterm_iconv_desc = (iconv_t)(-1);


/* Return a malloc()ed string converted using iconv().
 * If for_file_name is not 0, use the conversion defined for file names.
 * For NULL returns NULL. */
char *iconv_str (const iconv_t desc, const char *str)
{
	char buf[512];
#ifdef FREEBSD
	const char *inbuf;
#else
	char *inbuf;
#endif
	char *outbuf;
	char *str_copy;
	size_t inbytesleft, outbytesleft;
	char *converted;

	if (!str)
		return NULL;
	if (desc == (iconv_t)(-1))
		return xstrdup (str);

	inbuf = str_copy = xstrdup (str);
	outbuf = buf;
	inbytesleft = strlen(inbuf);
	outbytesleft = sizeof(buf) - 1;

	iconv (desc, NULL, NULL, NULL, NULL);

	while (inbytesleft) {
		if (iconv(desc, &inbuf, &inbytesleft, &outbuf,
					&outbytesleft)
				== (size_t)(-1)) {
			if (errno == EILSEQ) {
				inbuf++;
				inbytesleft--;
				if (!--outbytesleft) {
					*outbuf = 0;
					break;
				}
				*(outbuf++) = '#';
			}
			else if (errno == EINVAL) {
				*(outbuf++) = '#';
				*outbuf = 0;
				break;
			}
			else if (errno == E2BIG) {
				outbuf[sizeof(buf)-1] = 0;
				break;
			}
		}
	}

	*outbuf = 0;
	converted = xstrdup (buf);
	free (str_copy);

	return converted;
}

char *files_iconv_str (const char *str)
{
    return iconv_str (files_iconv_desc, str);
}
str files_iconv_str (const str &s)
{
	char *sc = files_iconv_str(s.c_str());
	str ret(sc);
	free(sc);
	return ret;
}

char *xterm_iconv_str (const char *str)
{
    return iconv_str (xterm_iconv_desc, str);
}

int xwaddstr (WINDOW *win, const char *str)
{
	int res;

	if (using_utf8)
		res = waddstr (win, str);
	else {
		char *lstr = iconv_str (iconv_desc, str);

		res = waddstr (win, lstr);
		free (lstr);
	}

	return res;
}
int xwaddstr (WINDOW *win, const str &s)
{
	int res;

	if (using_utf8)
		res = waddstr (win, s.c_str());
	else {
		char *lstr = iconv_str (iconv_desc, s.c_str());

		res = waddstr (win, lstr);
		free (lstr);
	}

	return res;
}

/* Convert multi-byte sequence to wide characters.  Change invalid UTF-8
 * sequences to '?'.  'dest' can be NULL as in mbstowcs().
 * If 'invalid_char' is not NULL it will be set to 1 if an invalid character
 * appears in the string, otherwise 0. */
static size_t xmbstowcs (wchar_t *dest, const char *src, size_t len,
		int *invalid_char)
{
	mbstate_t ps;
	size_t count = 0;

	assert (src != NULL);
	assert (!dest || len > 0);

	memset (&ps, 0, sizeof(ps));

	if (dest)
		memset (dest, 0, len * sizeof(wchar_t));

	if (invalid_char)
		*invalid_char = 0;

	while (src && (len || !dest)) {
		size_t res;

		res = mbsrtowcs (dest, &src, len, &ps);
		if (res != (size_t)-1) {
			count += res;
			src = NULL;
		}
		else {
			size_t converted;

			src++;
			if (dest) {
				converted = wcslen (dest);
				dest += converted;
				count += converted;
				len -= converted;

				if (len > 1) {
					*dest = L'?';
					dest++;
					*dest = L'\0';
					len--;
				}
				else
					*(dest - 1) = L'\0';
			}
			else
				count++;
			memset (&ps, 0, sizeof(ps));

			if (invalid_char)
				*invalid_char = 1;
		}
	}

	return count;
}

int xwaddnstr (WINDOW *win, const char *str, const int n)
{
	int res, width, inv_char;
	wchar_t *ucs;
	char *mstr, *lstr;
	size_t size, num_chars;

	if (n <= 0) return 0;
	assert (str != NULL);

	mstr = iconv_str (iconv_desc, str);

	size = xmbstowcs (NULL, mstr, -1, NULL) + 1;
	ucs = (wchar_t *)xmalloc (sizeof(wchar_t) * size);
	xmbstowcs (ucs, mstr, size, &inv_char);
	width = wcswidth (ucs, WIDTH_MAX);

	if (width == -1) {
		size_t clidx;
		for (clidx = 0; clidx < size - 1; clidx++) {
			if (wcwidth (ucs[clidx]) == -1)
				ucs[clidx] = L'?';
		}
		width = wcswidth (ucs, WIDTH_MAX);
		inv_char = 1;
	}

	if (width > n) {
		while (width > n)
			width -= wcwidth (ucs[--size]);
		ucs[size] = L'\0';
	}

	num_chars = wcstombs (NULL, ucs, 0);
	lstr = (char *)xmalloc (num_chars + 1);

	if (inv_char)
		wcstombs (lstr, ucs, num_chars + 1);
	else
		snprintf (lstr, num_chars + 1, "%s", mstr);

	res = waddstr (win, lstr);

	free (ucs);
	free (lstr);
	free (mstr);
	return res;
}

int xmvwaddstr (WINDOW *win, const int y, const int x, const char *str)
{
	int res;

	if (using_utf8)
		res = mvwaddstr (win, y, x, str);
	else {
		char *lstr = iconv_str (iconv_desc, str);

		res = mvwaddstr (win, y, x, lstr);
		free (lstr);
	}

	return res;
}
int xmvwaddstr (WINDOW *win, const int y, const int x, const str &s)
{
	int res;

	if (using_utf8)
		res = mvwaddstr (win, y, x, s.c_str());
	else {
		char *lstr = iconv_str (iconv_desc, s.c_str());

		res = mvwaddstr (win, y, x, lstr);
		free (lstr);
	}

	return res;
}

int xmvwaddnstr (WINDOW *win, const int y, const int x, const char *str,
		const int n)
{
	int res;

	if (using_utf8)
		res = mvwaddnstr (win, y, x, str, n);
	else {
		char *lstr = iconv_str (iconv_desc, str);

		res = mvwaddnstr (win, y, x, lstr, n);
		free (lstr);
	}

	return res;
}

int xwprintw (WINDOW *win, const char *fmt, ...)
{
	va_list va;
	int res;
	char *buf;

	va_start (va, fmt);
	buf = format_msg_va (fmt, va);
	va_end (va);

	if (using_utf8)
		res = waddstr (win, buf);
	else {
		char *lstr = iconv_str (iconv_desc, buf);

		res = waddstr (win, lstr);
		free (lstr);
	}

	free (buf);

	return res;
}

void xwprintfield(WINDOW *win, const str &s, int W, char ellipsis)
{
	int w = strwidth(s);
	if (w <= W)
	{
		xwaddstr(win, s.c_str());
		while (w++ < W) waddch (win, ' ');
		return;
	}
	
	if (W < 3)
	{
		xwaddnstr(win, s.c_str(), W);
		return;
	}

	switch (ellipsis)
	{
		case 'r':
			xwaddnstr(win, s.c_str(), W-3);
			xwaddstr(win, "...");
			break;
		case 'l':
		{
			char *t = xstrtail(s.c_str(), W-3);
			xwaddstr(win, "...");
			xwaddstr(win, t);
			free(t);
			break;
		}
		case 'c':
		{
			int l = (W-3)/2;
			xwaddnstr(win, s.c_str(), l);
			xwaddstr(win, "...");
			char *t = xstrtail(s.c_str(), W-3-l);
			xwaddstr(win, t);
			free(t);
			break;
		}
		default: assert(false);
	}
}

void clear_area (WINDOW *win, const Rect &r)
{
	str line = spaces(r.w);
	wattrset (win, get_color(CLR_BACKGROUND));
	for (int i = 0; i < r.h; ++i)
		xmvwaddstr(win, r.y+i, r.x, line.c_str());
}

static void iconv_cleanup ()
{
	if (iconv_desc != (iconv_t)(-1)
			&& iconv_close(iconv_desc) == -1)
		log_errno ("iconv_close() failed", errno);
}

void utf8_init ()
{
#ifdef HAVE_NL_LANGINFO_CODESET
#ifdef HAVE_NL_LANGINFO
	terminal_charset = xstrdup (nl_langinfo(CODESET));
	assert (terminal_charset != NULL);

	if (!strcmp(terminal_charset, "UTF-8")) {
		logit ("Using UTF8 output");
		using_utf8 = 1;
	}
	else
		logit ("Terminal character set: %s", terminal_charset);
#else /* HAVE_NL_LANGINFO */
	terminal_charset = xstrdup ("US-ASCII");
	logit ("Assuming US-ASCII terminal character set");
#endif /* HAVE_NL_LANGINFO */
#endif /* HAVE_NL_LANGINFO_CODESET */

	if (!using_utf8 && terminal_charset) {
		iconv_desc = iconv_open (terminal_charset, "UTF-8");
		if (iconv_desc == (iconv_t)(-1))
			log_errno ("iconv_open() failed", errno);
	}

	if (options::FileNamesIconv)
		files_iconv_desc = iconv_open ("UTF-8", "");
}

void utf8_cleanup ()
{
	if (terminal_charset)
		free (terminal_charset);
	iconv_cleanup ();
}

/* Return the number of columns the string occupies when displayed. */
size_t strwidth (const char *s)
{
	assert (s != NULL);

	size_t size = xmbstowcs (NULL, s, -1, NULL) + 1;
	wchar_t *ucs = (wchar_t *)xmalloc (sizeof(wchar_t) * size);
	xmbstowcs (ucs, s, size, NULL);
	size_t width = wcswidth (ucs, WIDTH_MAX);
	free (ucs);

	return width;
}
size_t strwidth (const str &s)
{
	size_t size = xmbstowcs (NULL, s.c_str(), -1, NULL) + 1;
	std::vector<wchar_t> ucs(size);
	xmbstowcs (ucs.data(), s.c_str(), size, NULL);
	return wcswidth (ucs.data(), WIDTH_MAX);
}

void strdel(str &s, int i, int n)
{
	if (i < 0) { n += i; i = 0; }
	if (n <= 0) return;

	size_t size = xmbstowcs (NULL, s.c_str(), -1, NULL) + 1;
	std::vector<wchar_t> ucs(size);
	xmbstowcs (ucs.data(), s.c_str(), size, NULL);
	int N = wcswidth (ucs.data(), WIDTH_MAX);
	if (i >= N) return;
	if (i + n > N) n = N-i;
	if (n <= 0) return;

	// TODO - horrible hack for now
	int k = 0; // initial part to keep
	if (i > 0) while (wcswidth(ucs.data()+k, WIDTH_MAX) > N-i) ++k;
	int l = k+1; while (wcswidth(ucs.data()+l, WIDTH_MAX) > N-i-n) ++l;
	ucs.erase(ucs.begin()+k, ucs.begin()+l);
	wcstombs((char*)s.data(), ucs.data(), s.length()+1);
}
int strins(str &s, int i, wchar_t c)
{
	size_t size = xmbstowcs (NULL, s.c_str(), -1, NULL) + 1;
	std::vector<wchar_t> ucs(size);
	xmbstowcs (ucs.data(), s.c_str(), size, NULL);
	int N = wcswidth (ucs.data(), WIDTH_MAX);
	if (i >= N)
	{
		ucs.back() = c;
		ucs.push_back(0);
	}
	else if (i <= 0)
	{
		ucs.insert(ucs.begin(), c);
	}
	else
	{
		int k = 0; // initial part to keep
		while (wcswidth(ucs.data()+k, WIDTH_MAX) > N-i) ++k;
		ucs.insert(ucs.begin()+k, c);
	}
	s.resize(s.length() + 16); // TODO
	wcstombs((char*)s.data(), ucs.data(), s.length());
	s.shrink_to_fit();
	return wcwidth(c);
}

/* Return a malloc()ed string containing the tail of 'str' up to a
 * maximum of 'len' characters (in columns occupied on the screen). */
char *xstrtail (const char *str, const int len)
{
	wchar_t *ucs;
	wchar_t *ucs_tail;
	size_t size;
	int width;
	char *tail;

	if (len <= 0) 
	{
		tail =(char *)xmalloc (1);
		*tail = 0;
		return tail;
	}
	assert (str != NULL);

	size = xmbstowcs(NULL, str, -1, NULL) + 1;
	ucs = (wchar_t *)xmalloc (sizeof(wchar_t) * size);
	xmbstowcs (ucs, str, size, NULL);
	ucs_tail = ucs;

	width = wcswidth (ucs, WIDTH_MAX);
	assert (width >= 0);

	while (width > len)
		width -= wcwidth (*ucs_tail++);

	size = wcstombs (NULL, ucs_tail, 0) + 1;
	tail = (char *)xmalloc (size);
	wcstombs (tail, ucs_tail, size);

	free (ucs);

	return tail;
}

str xstrtail (const str &s, int len)
{
	char *t = xstrtail(s.c_str(), len);
	str tt(t); free(t);
	return tt;
}