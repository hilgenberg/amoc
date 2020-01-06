/*
 * MOC - music on console
 * Copyright (C) 2002 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "menu.h"
#include "../rcc.h"
#include "utf8.h"
#include "../server/input/decoder.h"
#include "themes.h"
#include "interface.h"

static str sanitize(const str &s_)
{
	str s(s_);
	for (int i = 0, n = (int)s.length(); i < n; ++i)
	{
		if (s[i] != ' ' && isspace (s[i]))
		{
			s[i] = ' ';
		}
	}
	return s;
}

/* Convert time in second to min:sec text format. buff must be 6 chars long. */
static str sec_to_min(int sec)
{
	if (sec < 0) sec = 0;
	if (sec < 100*60)
		return format("%02d:%02d", sec / 60, sec % 60);
	sec /= 60;
	if (sec < 100*60)
		return format("%02dH%02d", sec / 60, sec % 60);
	sec /= 24;
	if (sec < 100*60)
		return format("%02dD%02d", sec / 24, sec % 24);
	return "++:++";
}

// distribute available screen width among columns
static void distribute(int W, int &c0, int &c1, int &c2)
{
	assert(W >= 0 && c0 >= 0 && c1 >= 0 && c2 >= 0);
	if (c0+c1+c2 <= W) return;

	// start with even split, then reclaim wasted space
	int w0 = W/3, w1 = w0, w2 = W-w0-w1;
	int o0 = c0-w0, o1 = c1-w1, o2 = c2-w2;
	if (o0 < 0 || o1 < 0 || o2 < 0)
	{
		int w = W;
		assert(o0 >= 0 || o1 >= 0 || o2 >= 0); // otherwise we would have returned already
		if (o0 < 0) { w0 = c0; w -= c0; }
		if (o1 < 0) { w1 = c1; w -= c1; }
		if (o2 < 0) { w2 = c2; w -= c2; }
		if      (o0 < 0 && o1 < 0) { w2 = w; }
		else if (o0 < 0 && o2 < 0) { w1 = w; }
		else if (o1 < 0 && o2 < 0) { w0 = w; }
		else if (o0 < 0)
		{
			w1 = w/2, w2 = w-w1;
			o1 = c1-w1; o2 = c2-w2;
			if      (o1 < 0) { w1 = c1; w2 = w-c1; }
			else if (o2 < 0) { w2 = c2; w1 = w-c2; }
		}
		else if (o1 < 0)
		{
			w0 = w/2, w2 = w-w0;
			o0 = c0-w0; o2 = c2-w2;
			if      (o0 < 0) { w0 = c0; w2 = w-c0; }
			else if (o2 < 0) { w2 = c2; w0 = w-c2; }
		}
		else if (o2 < 0)
		{
			w0 = w/2, w1 = w-w0;
			o0 = c0-w0; o1 = c1-w1;
			if      (o0 < 0) { w0 = c0; w1 = w-c0; }
			else if (o1 < 0) { w1 = c1; w0 = w-c1; }
		}
	}
	c0 = w0; c1 = w1; c2 = w2;
	assert(c0 >= 0 && c1 >= 0 && c2 >= 0);
	assert(c0+c1+c2 == W);
}

bool menu::mark_path(const str &f)
{
	mark = items.find(f);
	return mark >= 0;
}
bool menu::select_path(const str &f)
{
	int i = items.find(f);
	if (i >= 0)
	{
		sel = i;
		return true;
	}
	return false;
}

void menu::draw(bool active) const
{
	auto *win = iface->window();
	if (!win) return;

	const int N = items.size();
	const int asel = active ? sel : -1;

	bool have_up = items.is_dir && N && iface->cwd() != "/";

	// gather layout data: widths for three columns (artist, album, title)
	// and maximum track number
	int extra = 5 /*rating*/ + 5 /*time*/ + 3 /*[|]*/ + 3*2 /*spacing*/;
	int c0 = 0, c1 = 0, c2 = 0, M = 0;
	bool readtags = options::ReadTags;
	int avail = bounds.w - extra;
	if (avail-2 < 3*4) readtags = false;
	bool first = true;
	if (readtags) for (const auto &ip : items.items)
	{
		const auto &it = *ip;
		if (first) { first = false; if (have_up) continue; } // should not have tags anyway...

		if (!it.tags) continue;
		auto &tags = *it.tags;

		if (tags.title.empty())
		{
			continue;
		}
		else
		{
			M  = std::max(M,  tags.track);
			c0 = std::max(c0, (int)strwidth(sanitize(tags.artist)));
			c1 = std::max(c1, (int)strwidth(sanitize(tags.album)));
			c2 = std::max(c2, (int)strwidth(sanitize(tags.title)));
		}
	}

	int prefix_len = 0; // how much to cut off from file paths
	if (!items.is_dir)
	{
		first = true;
		str common_prefix; // of all paths (even those with tags!)
		for (const auto &ip : items.items)
		{
			const auto &it = *ip;
			if (it.type == F_URL) continue;
			if (first)
			{
				common_prefix = it.path;
				first = false;
			}
			else
			{
				intersect(common_prefix, it.path);
			}
		}
		prefix_len = (int)common_prefix.length();
		while (prefix_len > 0 && common_prefix[prefix_len-1] != '/') --prefix_len;
	}
	int cn = 0;
	if (!items.is_dir)
	{
		M = N;
		for (cn = 2, M /= 10; M > 0; M /= 10) ++cn;
	}
	else if (M > 0)
	{
		for (cn = 2, M /= 10; M > 0; M /= 10) ++cn;
	}
	avail -= cn;
	if (avail < 3*4)
	{
		readtags = false;
		c0 = c1 = c2 = 0;
		if (items.is_dir) cn = 0;
		extra = 0;
		avail = bounds.w - cn;
	}
	if (avail < 6)
	{
		wattrset (win, get_color(CLR_MENU_ITEM_FILE));
		str s(":::TOO SMALL:::");
		for (int y = 0; y < bounds.h; ++y)
		{
			int i = y-(bounds.h-1)/2 + s.length()/2;
			if (i < 0 || i >= s.length()) continue;
			mvwaddch(win, bounds.y + y, bounds.x+(bounds.w-1)/2, s[i]);
		}
		return;
	}

	if (readtags) distribute(avail, c0, c1, c2);
	assert(c0+c1+c2+cn+extra <= bounds.w);
	
	// draw the visible items
	for (int i = top, n = std::min(top + bounds.h, N); i < n; ++i)
	{
		bool is_up_dir = (have_up && i == 0); // is it ".." ?

		const auto &it = *items.items[i];
		int y = bounds.y + i-top, x0 = bounds.x, x1 = bounds.x + bounds.w-1;

		int info_color = get_color(
			i == asel && i == mark ? CLR_MENU_ITEM_INFO_MARKED_SELECTED :
			i == asel ? CLR_MENU_ITEM_INFO_SELECTED :
			i == mark ? CLR_MENU_ITEM_INFO_MARKED :
			CLR_MENU_ITEM_INFO);
		int file_color = get_color(
			i == asel && i == mark ? CLR_MENU_ITEM_FILE_MARKED_SELECTED :
			i == mark ? CLR_MENU_ITEM_FILE_MARKED :
			it.type == F_DIR ? (i == asel ? CLR_MENU_ITEM_DIR_SELECTED : CLR_MENU_ITEM_DIR) :
			it.type == F_PLAYLIST ? (i == asel ? CLR_MENU_ITEM_PLAYLIST_SELECTED : CLR_MENU_ITEM_PLAYLIST) :
			i == asel ? CLR_MENU_ITEM_FILE_SELECTED : CLR_MENU_ITEM_FILE);
		
		wattrset (win, info_color);

		// playlist numbering
		if (!items.is_dir && cn)
		{
			wmove (win, y, x0);
			xwprintw(win, "%*d ", cn-1, i+1);
			x0 += cn;
		}

		// rating and time
		if (readtags && it.type != F_DIR)
		{
			x1 -= 13;
			mvwaddstr(win, y, x1+1, "[");
		
			wattrset (win, file_color);
			wattroff(win, A_BOLD); // some utf8 chars do not have bold versions
			int rating = (it.tags ? it.tags->rating : 0);
			if (rating < 0) rating = 0;
			if (rating > 5) rating = 5;

			xwaddstr(win, options::rating_strings[rating]);

			wattrset (win, info_color);
			waddch(win, '|');
			if (it.tags && it.tags->time > -1)
			{
				xwaddstr(win, sec_to_min(it.tags->time));
			}
			else
			{
				xwaddstr(win, it.type == F_URL ? "--:--" : "     ");
			}
			waddch(win, ']');
		}

		wattrset (win, file_color);
		wmove (win, y, x0);

		if (!is_up_dir && readtags && it.tags && !it.tags->title.empty())
		{
			auto &tags = *it.tags;
			xwprintfield(win, sanitize(tags.artist), c0);
			waddstr(win, "   ");
			xwprintfield(win, sanitize(tags.album), c1);
			waddstr(win, "   ");
			if (items.is_dir)
			{
				if (tags.track > 0)
					xwprintw(win, "%*d ", cn-1, tags.track);
				else
					xwprintw(win, "%*s", cn, "");
			}
			xwprintfield(win, sanitize(tags.title), c2);
		}
		else if (it.type == F_URL)
		{
			xwprintfield(win, sanitize(it.path), x1-x0+1, 'c');
		}
		else if (is_up_dir && it.type == F_DIR)
		{
			xwprintfield(win, "../", x1-x0+1, 'l');
		}
		else
		{
			str s = is_up_dir ? str("..") : it.path;
			if (prefix_len > 0 && it.type != F_DIR) s = s.substr(prefix_len);
			s = sanitize(s);
			if (it.type == F_DIR && s.back() != '/') s += '/';
			if (items.is_dir || !options::PlaylistFullPaths)
			{
				auto i = s.rfind('/', s.length()-2);
				if (i != str::npos) s = s.substr(i+1);
			}
			if (options::HideFileExtension)
			{
				const char *file = s.c_str(), *ext = ext_pos(file);
				if (ext) s = s.substr(0, ext-1-file);
			}
			if (options::FileNamesIconv)
				s = files_iconv_str (s);
			#ifdef  HAVE_RCC
			if (options::UseRCCForFilesystem)
				s = rcc_reencode(s);
			#endif
			xwprintfield(win, s, x1-x0+1, 'l');
		}
	}
}
