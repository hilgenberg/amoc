#pragma once

class View
{
public:
	virtual void draw() const = 0;

	// return true to capture mouse events until mouse-up
	virtual bool  start_drag(int x, int y) { return false; }
	virtual void handle_drag(int x, int y) {}
	virtual void finish_drag(int x, int y) {}

	// return false if the event is handled and noone else needs to see it
	virtual bool handle_click(int x, int y, bool dbl) { return false; }

	virtual bool handle_scroll(int x, int y, int dy) { return false; }
};
