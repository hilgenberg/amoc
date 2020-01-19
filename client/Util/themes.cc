/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "themes.h"
#include <ncurses.h>
#include <iostream>
#include <fstream>
using namespace std;

/* ncurses extension */
#ifndef COLOR_DEFAULT
#define COLOR_DEFAULT	-2
#endif

/* hidden color? */
#ifndef COLOR_GREY
#define COLOR_GREY	10
#endif

static int colors[CLR_LAST];
int get_color (color_index i) { return colors[i]; }

/* Initialize a color item of given index (CLR_*) with colors and
 * attributes. Do nothing if the item is already initialized. */
static void make_color (color_index i, short foreground, short background, attr_t attr)
{
	static short pair_count = 1;
	assert (pair_count < COLOR_PAIRS);
	assert (i < CLR_LAST);

	if (colors[i] == -1) {
		init_pair (pair_count, foreground, background);
		colors[i] = COLOR_PAIR (pair_count) | attr;
		++pair_count;
	}
}

static void set_default_colors ()
{
	make_color (CLR_BACKGROUND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_FRAME, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_WIN_TITLE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_PANEL_DIR, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_PANEL_DIR_SELECTED, COLOR_WHITE, COLOR_BLACK, A_BOLD);
	make_color (CLR_PANEL_PLAYLIST, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_PANEL_PLAYLIST_SELECTED, COLOR_WHITE, COLOR_BLACK, A_BOLD);
	make_color (CLR_PANEL_FILE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_PANEL_FILE_SELECTED, COLOR_WHITE, COLOR_BLACK, A_NORMAL);
	make_color (CLR_PANEL_FILE_MARKED, COLOR_GREEN, COLOR_BLUE, A_BOLD);
	make_color (CLR_PANEL_FILE_MARKED_SELECTED, COLOR_GREEN, COLOR_BLACK, A_BOLD);
	make_color (CLR_PANEL_INFO, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_PANEL_INFO_SELECTED, COLOR_BLUE, COLOR_BLACK, A_BOLD);
	make_color (CLR_PANEL_INFO_MARKED, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_PANEL_INFO_MARKED_SELECTED, COLOR_BLUE, COLOR_BLACK, A_BOLD);
	make_color (CLR_STATUS, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TITLE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_STATE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_CURRENT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_LEFT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_TOTAL_FRAMES, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TIME_TOTAL, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_SOUND_PARAMS, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_LEGEND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_INFO_DISABLED, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_INFO_ENABLED, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MIXER_BAR_EMPTY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_MIXER_BAR_FILL, COLOR_BLACK, COLOR_CYAN, A_NORMAL);
	make_color (CLR_TIME_BAR_EMPTY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TIME_BAR_FILL, COLOR_BLACK, COLOR_CYAN, A_NORMAL);
	make_color (CLR_ENTRY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_ENTRY_TITLE, COLOR_BLACK, COLOR_CYAN, A_BOLD);
	make_color (CLR_ERROR, COLOR_RED, COLOR_BLUE, A_BOLD);
	make_color (CLR_MESSAGE, COLOR_GREEN, COLOR_BLUE, A_BOLD);
	make_color (CLR_PLIST_TIME, COLOR_WHITE, COLOR_BLUE, A_NORMAL);

	make_color (CLR_MENU_ITEM, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MENU_SELECTED, COLOR_WHITE, COLOR_BLACK, A_BOLD);
	make_color (CLR_MENU_GREY, COLOR_YELLOW, COLOR_BLUE, A_BOLD);
	make_color (CLR_MENU_GREY_SELECTED, COLOR_RED, COLOR_BLACK, A_BOLD);
}

/* Find the index of a color element by name. Return CLR_LAST if not found. */
static enum color_index find_color_element_name (const char *name)
{
	static struct
	{
		const char *name;
		enum color_index idx;
	} color_tab[] = {
		{ "background",		CLR_BACKGROUND },
		{ "frame",		CLR_FRAME },
		{ "window_title",	CLR_WIN_TITLE },
		{ "directory",		CLR_PANEL_DIR },
		{ "selected_directory", CLR_PANEL_DIR_SELECTED },
		{ "playlist",		CLR_PANEL_PLAYLIST },
		{ "selected_playlist",	CLR_PANEL_PLAYLIST_SELECTED },
		{ "file",		CLR_PANEL_FILE },
		{ "selected_file",	CLR_PANEL_FILE_SELECTED },
		{ "marked_file",	CLR_PANEL_FILE_MARKED },
		{ "marked_selected_file", CLR_PANEL_FILE_MARKED_SELECTED },
		{ "info",		CLR_PANEL_INFO },
		{ "selected_info",	CLR_PANEL_INFO_SELECTED },
		{ "marked_info",	CLR_PANEL_INFO_MARKED },
		{ "marked_selected_info", CLR_PANEL_INFO_MARKED_SELECTED },

		{ "menu_item",		CLR_MENU_ITEM },
		{ "menu_selected",	CLR_MENU_SELECTED },
		{ "menu_greyed",	CLR_MENU_GREY },
		{ "menu_greyed_selected",CLR_MENU_GREY_SELECTED },

		{ "status",		CLR_STATUS },
		{ "title",		CLR_TITLE },
		{ "state",		CLR_STATE },
		{ "current_time",	CLR_TIME_CURRENT },
		{ "time_left",		CLR_TIME_LEFT },
		{ "total_time",		CLR_TIME_TOTAL },
		{ "time_total_frames",	CLR_TIME_TOTAL_FRAMES },
		{ "sound_parameters",	CLR_SOUND_PARAMS },
		{ "legend",		CLR_LEGEND },
		{ "disabled",		CLR_INFO_DISABLED },
		{ "enabled",		CLR_INFO_ENABLED },
		{ "empty_mixer_bar",	CLR_MIXER_BAR_EMPTY },
		{ "filled_mixer_bar",	CLR_MIXER_BAR_FILL },
		{ "empty_time_bar",	CLR_TIME_BAR_EMPTY },
		{ "filled_time_bar",	CLR_TIME_BAR_FILL },
		{ "entry",		CLR_ENTRY },
		{ "entry_title",	CLR_ENTRY_TITLE },
		{ "error",		CLR_ERROR },
		{ "message",		CLR_MESSAGE },
		{ "plist_time",		CLR_PLIST_TIME }
	};

	assert (name != NULL);

	for (int ix = 0; ix < ARRAY_SIZE(color_tab); ix += 1) {
		if (!strcasecmp(color_tab[ix].name, name))
			return color_tab[ix].idx;
	}

	return CLR_LAST;
}

/* Find the curses color by name. Return -1 if the color is unknown. */
static short find_color_name (const char *name)
{
	static struct
	{
		const char *name;
		short color;
	} color_tab[] = {
		{ "black",	COLOR_BLACK },
		{ "red",	COLOR_RED },
		{ "green",	COLOR_GREEN },
		{ "yellow",	COLOR_YELLOW },
		{ "blue",	COLOR_BLUE },
		{ "magenta",	COLOR_MAGENTA },
		{ "cyan",	COLOR_CYAN },
		{ "white",	COLOR_WHITE },
		{ "default",	COLOR_DEFAULT },
		{ "grey",	COLOR_GREY }
	};

	for (int ix = 0; ix < ARRAY_SIZE(color_tab); ix += 1) {
		if (!strcasecmp(color_tab[ix].name, name))
			return color_tab[ix].color;
	}

	return -1;
}

/* The lines should be in format:
 *
 * ELEMENT = FOREGROUND BACKGROUND [ATTRIBUTE[,ATTRIBUTE,..]]
 *
 * Blank lines and beginning with # are ignored, see example_theme.
 */
static bool parse_theme_line (const int line_num, str &line)
{
	const char *s = line.c_str();
	while (isspace(*s)) ++s;
	if (!*s || *s == '#') return true;

	#define FAIL(msg) do{\
		interface_fatal("Parse error in theme file line %d: %s", line, msg);\
		return false; }while(0)

	int k = 0;
	while (s[k] && s[k] != '=' && !isspace(s[k])) ++k;
	if (k == 0) FAIL("missing element name");
	str name(s, s+k); s += k;
	color_index element = find_color_element_name(name.c_str());
	if (element == CLR_LAST) FAIL("unknown element");
	
	while (isspace(*s)) ++s;
	if (*s != '=') FAIL("expected '=' missing");
	++s;

	while (isspace(*s)) ++s;
	k = 0; while(s[k] && !isspace(s[k])) ++k;
	if (k == 0) FAIL("foreground color not specified");
	short fg = find_color_name(str(s, s+k).c_str()); s += k;
	if (fg == -1) FAIL("bad foreground color name");

	while (isspace(*s)) ++s;
	k = 0; while(s[k] && !isspace(s[k])) ++k;
	short bg = k == 0 ? COLOR_DEFAULT : find_color_name(str(s, s+k).c_str()); s += k;
	if (bg == -1) FAIL("bad background color name");

	attr_t curses_attr = 0;
	bool want_comma = false;
	while (true)
	{
		while (isspace(*s)) ++s;
		if (want_comma) {
			if (*s != ',') FAIL("missing comma");
			want_comma = false; ++s;
			while (isspace(*s)) ++s;
		}
		k = 0; while(s[k] && !isspace(s[k]) && s[k] != ',') ++k;
		if (k == 0) { if (s[k]) FAIL("too many commas"); else break; }
		str tmp(s, s+k); s += k; const char *attr = tmp.c_str();
		if      (!strcasecmp(attr,    "normal")) curses_attr |= A_NORMAL;
		else if (!strcasecmp(attr,  "standout")) curses_attr |= A_STANDOUT;
		else if (!strcasecmp(attr, "underline")) curses_attr |= A_UNDERLINE;
		else if (!strcasecmp(attr,   "reverse")) curses_attr |= A_REVERSE;
		else if (!strcasecmp(attr,     "blink")) curses_attr |= A_BLINK;
		else if (!strcasecmp(attr,       "dim")) curses_attr |= A_DIM;
		else if (!strcasecmp(attr,      "bold")) curses_attr |= A_BOLD;
		else if (!strcasecmp(attr,   "protect")) curses_attr |= A_PROTECT;
		else FAIL(format("unknown attribute: \"%s\"", attr).c_str());

		while (isspace(*s)) ++s;
		if (!*s) break;
		want_comma = true;
	}
	#undef FAIL

	make_color (element, fg, bg, curses_attr);

	return true;
}

void theme_init ()
{
	for (int i = 0; i < CLR_LAST; i++) colors[i] = -1;

	auto path = options::config_file_path("colors");
	if (file_exists(path))
	{
		ifstream f(path);
		if (!f.is_open()) interface_fatal("Can't open theme file: %s", xstrerror(errno));

		string line; int line_num = 0;
		while (getline(f, line)) if (!parse_theme_line(++line_num, line)) break;

		if (f.bad()) interface_fatal("Error reading theme file: %s", xstrerror(errno));
	}

	set_default_colors ();
}
