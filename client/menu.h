#pragma once

#include <ncurses.h>
#include "../playlist.h"
#include "Rect.h"

enum menu_request
{
	REQ_UP,
	REQ_DOWN,
	REQ_XUP,
	REQ_XDOWN,
	REQ_PGUP,
	REQ_PGDOWN,
	REQ_TOP,
	REQ_BOTTOM
};

class Interface;

struct menu
{
	// c'tor gets  called before the interface is running!
	menu(Interface *iface, plist &items) : iface(iface), items(items), top(0), sel(-1), xsel(0), mark(-1) {}

	void draw(bool active) const; // no frame, draws just the inside

	void move(menu_request req); // move selection
	void handle_click(int x, int y, bool dbl);

	bool mark_path(const str &f); // or unmark if not found
	void mark_item(int i); // this and mark_path take the selection along if sel==mark
	bool select_path(const str &f); // leave selection as is if not found
	void select_item(int i);
	bool item_visible (int i) const { return i >= top && i < top + bounds.h; };

	Interface *iface;
	plist  &items;
	Rect    bounds;
	mutable int top; // first visible item
	mutable int sel, mark; // selected and marked items, -1 if none
	mutable int xsel; // if != 0: multi-sel from sel to sel+xsel (both inclusive)
};
