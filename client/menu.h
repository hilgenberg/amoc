#pragma once

#include <ncurses.h>
#include "../playlist.h"
#include "Rect.h"

enum menu_request
{
	REQ_UP,
	REQ_DOWN,
	REQ_PGUP,
	REQ_PGDOWN,
	REQ_TOP,
	REQ_BOTTOM
};

class Interface;

struct menu
{
	// c'tor gets  called before the interface is running!
	menu(Interface *iface, plist &items) : iface(iface), items(items), top(0), sel(-1), mark(-1) {}

	void draw(bool active) const; // no frame, draws just the inside

	void move(menu_request req) // move selection
	{
		const int N = items.size();
		if (!N) return;
		if (sel < 0 && mark >= 0 && mark < N) sel = mark;
		switch (req)
		{
			case REQ_UP:     sel = (sel < 0 ? N-1 : sel - 1); break;
			case REQ_DOWN:   sel = (sel < 0 ? 0 : sel + 1); break;
			case REQ_PGUP:
				top -= (bounds.h - 1); if (top < 0) top = 0;
				sel -= (bounds.h - 1);
				break;
			case REQ_PGDOWN:
				top += (bounds.h - 1);
				sel += (bounds.h - 1);
				if (top + (bounds.h-1) >= N) top = N - bounds.h;
				if (top < 0) top = 0;
				break;
			case REQ_TOP:    sel = 0; break;
			case REQ_BOTTOM: sel = N-1; break;
		}
		if (sel <  0) sel = 0;
		if (sel >= N) sel = N-1;
	}

	plist_item *current_item () const // selected (or marked if none) item
	{
		const auto N = items.size();
		if (sel >= 0 && sel < N) return items.items[sel].get();
		if (mark >= 0 && mark < N) return items.items[mark].get();
		return NULL;
	}
	bool mark_path(const str &f); // or unmark if not found
	void mark_item(int i); // this and mark_path take the selection along if sel==mark
	bool select_path(const str &f); // leave selection as is if not found
	bool item_visible (int i) const { return i >= top && i < top + bounds.h; };

	Interface *iface;
	plist  &items;
	Rect    bounds;
	mutable int top; // first visible item
	mutable int sel, mark; // selected and marked items, -1 if none
};
