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
#include <codecvt>
#include <locale>

typedef std::wstring wstr;
#define WIDTH_MAX std::numeric_limits<size_t>::max() // parameter for wcswidth

// is width == length?
static inline bool is_ascii(const str &s)
{
	for (char c : s)
		if ((unsigned char)c < 32 || (unsigned char)c >= 127)
			return false;
	return true;
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
		else if (k == (size_t)-1)
		{
			ws.push_back(L'?');
			--n;
			++s;
			memset (&ps, 0, sizeof(ps));
		}
		else if (k == (size_t)-2)
		{
			// trailing garbage
			ws.push_back(L'?');
			break;
		}
		else
		{
			ws.push_back(wcwidth(c) >= 0 ? c : L'?');
			n -= k;
			s += k;
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
	if (is_ascii(s)) return s.length();

	wstr ws = safe_convert(s);
	size_t n = wcswidth(ws.c_str(), WIDTH_MAX);
	if (n != (size_t)-1) return n;
	
	assert(false); // safe_convert should have sanitized the string
	n = 0;
	for (const wchar_t &c : ws) { auto w = wcwidth(c); if (w < 0) ++n; else n += w; }
	return n;
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
		assert(cw >= 0);
		i -= cw;
	}
	if (j >= m) return;

	int k = 0;
	while (n > 0 && j+k < m)
	{
		int cw = wcwidth(ws[j + k++]);
		assert(cw >= 0);
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
			assert(cw >= 0);
			i -= cw;
		}
		ws.insert(j, 1, c);
	}
	s = convert(ws);
	return wcwidth(c);
}

/* Return a string containing the tail of 'str' up to a
 * maximum of len characters (in columns occupied on the screen). */
str strtail (const str &s, int len)
{
	if (len <= 0) return str();
	if (is_ascii(s)) return s.length() <= len ? s : s.substr(s.length() - len);

	wstr ws = safe_convert(s);
	int k = (int)ws.length();
	while (len > 0 && k > 0)
	{
		int cw = wcwidth(ws[k-1]);
		if (cw > len) break;
		assert(cw >= 0);
		len -= cw; --k;
	}
	while (k > 0)
	{
		// add zero-length chars
		int cw = wcwidth(ws[k-1]);
		if (cw != 0) break;
		--k;
	}
	return convert(ws.substr(k));
}
str strhead (const str &s, int len)
{
	if (len <= 0) return str();
	if (is_ascii(s)) return s.length() <= len ? s : s.substr(0, len);

	wstr ws = safe_convert(s);
	int w = 0, j = 0, m = ws.length();
	while (w < len && j < m)
	{
		int cw = wcwidth(ws[j++]);
		assert(cw >= 0);
		w += cw;
	}
	return convert(j < m ? ws.substr(0, j) : ws);
}
