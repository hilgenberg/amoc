#pragma once

#include <ncurses.h>
#include "../../playlist.h"
#include "../Util/Rect.h"
#include "View.h"

enum menu_request
{
	REQ_UP,
	REQ_DOWN,
	REQ_XUP,
	REQ_XDOWN,
	REQ_SCROLL_UP,
	REQ_SCROLL_DOWN,
	REQ_PGUP,
	REQ_PGDOWN,
	REQ_TOP,
	REQ_BOTTOM,
	REQ_COLLAPSE // collapse multi-selection to a single selection
};

class Interface;

//---------------------------------------------------------------
// Panel draws the playlist views and handles selection, current
// song mark and various events. Closely related to FrameView.
//---------------------------------------------------------------

struct Panel : public View
{
	// c'tor gets  called before the interface is running!
	Panel(Interface &iface, plist &items) : iface(iface), items(items), top(0), sel(-1), xsel(0), mark(-1) {}

	void set_active(bool a) { active = a; }
	void draw() const override; // no frame, draws just the inside

	void move_selection(menu_request req);
	bool handle_click(int x, int y, bool dbl) override;

	bool mark_path(const str &f); // or unmark if not found
	void mark_item(int i); // this and mark_path take the selection along if sel==mark
	bool select_path(const str &f); // leave selection as is if not found
	void select_item(int i);
	bool item_visible (int i) const { return i >= top && i < top + bounds.h; };

	Interface  &iface;
	plist      &items;
	Rect        bounds;
	bool        active;
	mutable int top; // first visible item
	mutable int sel, mark; // selected and marked items, -1 if none
	mutable int xsel; // if != 0: multi-sel from sel to sel+xsel (both inclusive)
};
