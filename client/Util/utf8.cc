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


#include "utf8.h"
#include "../../rcc.h"

#include <iconv.h>
#include <nl_types.h>
#include <langinfo.h>
#include <ncurses.h>
#include <wchar.h>
#include <codecvt>
#include <string>
#include <locale>

typedef std::wstring wstr;
#define WIDTH_MAX std::numeric_limits<size_t>::max() // parameter for wcswidth

static bool    using_utf8 = false; // terminal is UTF-8 and term_iconv_desc is not needed?
static iconv_t term_iconv_desc = (iconv_t)(-1); // from UTF-8 to terminal
static iconv_t files_iconv_desc = (iconv_t)(-1); // from auto-detect to UTF-8

str iconv_str(const iconv_t desc, const str &input)
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
str files_iconv_str (const str &s)
{
	return iconv_str(files_iconv_desc, s);
}

// convert possibly broken utf8 to wstr and replace all characters
// having wcwidth < 0 with '?'
static wstr safe_convert(const str &src)
{
	wstr ws;
	const char *s = src.c_str();

	// optimize for the case where src contains no garbage
	size_t count = mbstowcs(NULL, s, 0);
	if (count != (size_t)-1)
	{
		ws.resize(count);
		mbstowcs(const_cast<wchar_t*>(ws.c_str()), s, count+1);
		for (auto &c : ws) if (wcwidth(c) < 0) c = '?';
		return ws;
	}

	// if it does contain garbage, replace that with '?' as well
	mbstate_t ps; memset (&ps, 0, sizeof(ps));
	size_t n = src.length();
	while (n)
	{
		wchar_t c;
		size_t k = mbrtowc(&c, s, n, &ps);
		if (k == 0)
		{
			break;
		}
		else if (k > 0)
		{
			ws.push_back(wcwidth(c) >= 0 ? c : L'?');
			n -= k;
			s += k;
		}
		else if (k == (size_t)-1)
		{
			ws.push_back(L'?');
			--n;
			++s;
		}
		else if (k == (size_t)-2)
		{
			// trailing garbage
			ws.push_back(L'?');
			break;
		}
		else
		{
			assert(false);
			ws.push_back(L'#');
			break;
		}
	}

	return ws;
}
/*static wstr convert(const str &s) // utf8 to wstr, throws on invalid sequences
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> c;
    return c.from_bytes(s);
}*/

static str convert(const wstr &s) // back to utf8
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> c;
	return c.to_bytes(s);
}

str sanitized(const str &s_)
{
	str s = convert(safe_convert(s_));
	for (char &c : s) if (c != ' ' && isspace((unsigned char)c)) c = ' ';
	return s;
}
void sanitize(str &s)
{
	s = convert(safe_convert(s));
	for (char &c : s) if (c != ' ' && isspace((unsigned char)c)) c = ' ';
}

/* Return the number of columns the string occupies when displayed */
size_t strwidth (const str &s)
{
	wstr ws = safe_convert(s);
	size_t n = wcswidth(ws.c_str(), WIDTH_MAX);
	if (n != (size_t)-1) return n;

	for (wchar_t &c : ws) if (wcwidth(c) == -1) c = L'?';
	return wcswidth (ws.c_str(), WIDTH_MAX);
}

void strdel(str &s, int i, int n)
{
	if (i < 0) { n += i; i = 0; }
	if (n <= 0) return;

	wstr ws = safe_convert(s);

	int j = 0, m = ws.length();
	if (i >= m) return;

	while (j < m && i > 0)
	{
		int cw = wcwidth(ws[j++]);
		if (cw <= 0) cw = 1; // gets printed as '?'
		i -= cw;
	}
	if (j >= m) return;

	int k = 0;
	while (n > 0 && j+k < m)
	{
		int cw = wcwidth(ws[j + k++]);
		if (cw <= 0) cw = 1; // gets printed as '?'
		n -= cw;
	}
	ws.erase(j, k);

	s = convert(ws);
}
int strins(str &s, int i, wchar_t c)
{
	wstr ws = safe_convert(s);
	if (i <= 0)
		ws.insert(0, 1, c);
	else if (i > ws.length())
		// if i > m, then i > strwidth(s) too
		ws.push_back(c);
	else
	{
		int j = 0, m = ws.length();

		while (j < m && i > 0)
		{
			int cw = wcwidth(ws[j++]);
			if (cw <= 0) cw = 1; // gets printed as '?'
			i -= cw;
		}
		ws.insert(j, 1, c);
	}
	s = convert(ws);
	return wcwidth(c);
}

/* Return a string containing the tail of 'str' up to a
 * maximum of len characters (in columns occupied on the screen). */
str xstrtail (const str &s, int len)
{
	if (len <= 0) return str();

	wstr ws = safe_convert(s);
	int w = wcswidth (ws.c_str(), WIDTH_MAX);
	if (w <= len) return s;

	int k = 0, m = ws.length();
	while (w > len && k < m)
	{
		int cw = wcwidth(ws[k++]);
		if (cw <= 0) cw = 1; // gets printed as '?'
		w -= cw;
	}
	if (w > len) { assert(false); return str(); }
	return convert(ws.substr(k));
}


#define TO_TERM(s) (using_utf8 ? (s).c_str() : iconv_str(term_iconv_desc, s).c_str())

int xwaddstr (WINDOW *win, const str &s)
{
	return waddstr(win, TO_TERM(s));
}

static int xwaddnstr (WINDOW *win, const str &s, int len)
{
	if (len <= 0) return 0;

	wstr ws = safe_convert(s);
	int w = 0, j = 0, m = ws.length();
	while (w < len && j < m)
	{
		int cw = wcwidth(ws[j++]);
		if (cw <= 0) { ws[j-1] = L'?'; cw = 1; }
		w += cw;
	}
	return xwaddstr(win, convert(j < m ? ws.substr(0, j) : ws));
}

void xwprintfield(WINDOW *win, const str &s, int W, char fmt)
{
	int w = strwidth(s);
	if (w <= W)
	{
		if (fmt == 'R')
		{
			while (w++ < W) waddch (win, ' ');
			xwaddstr(win, s);
		}
		else if (fmt == 'C')
		{
			int k = W-w;
			for (int i = 0; i < k/2; ++i) waddch (win, ' ');
			xwaddstr(win, s);
			for (int i = k/2; i < k; ++i) waddch (win, ' ');
		}
		else
		{
			xwaddstr(win, s);
			while (w++ < W) waddch (win, ' ');
		}
		return;
	}
	
	if (W < 3)
	{
		xwaddnstr(win, s, W);
		return;
	}

	switch (fmt)
	{
		case 'r':
		case 'C':
			xwaddnstr(win, s, W-3);
			xwaddstr(win, "...");
			break;
		case 'l':
		case 'R':
		{
			xwaddstr(win, "...");
			xwaddstr(win, xstrtail(s, W-3));
			break;
		}
		case 'c':
		{
			int l = (W-3)/2;
			xwaddnstr(win, s, l);
			xwaddstr(win, "...");
			xwaddstr(win, xstrtail(s, W-3-l));
			break;
		}
		default: assert(false);
	}
}

void utf8_init ()
{
	const char *terminal_charset = nl_langinfo(CODESET);
	assert (terminal_charset != NULL);

	if (!strcmp(terminal_charset, "UTF-8")) {
		logit ("Using UTF8 output");
		using_utf8 = true;
	}
	else
		logit ("Terminal character set: %s", terminal_charset);

	if (!using_utf8 && terminal_charset) {
		term_iconv_desc = iconv_open (terminal_charset, "UTF-8");
		if (term_iconv_desc == (iconv_t)(-1)) log_errno ("iconv_open() failed", errno);
	}

	if (options::FileNamesIconv)
		files_iconv_desc = iconv_open ("UTF-8", "");
}

void utf8_cleanup ()
{
	if (term_iconv_desc != (iconv_t)-1 && iconv_close(term_iconv_desc) == -1)
		log_errno ("iconv_close() failed", errno);
	if (files_iconv_desc != (iconv_t)-1 && iconv_close(files_iconv_desc) == -1)
		log_errno ("iconv_close() failed", errno);
}
