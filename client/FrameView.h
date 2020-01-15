#pragma once
#include "View.h"
class Interface;

//---------------------------------------------------------------
// FrameView draws the borders around the two panels and the
// info area as well as the total playlist times and status
// messages (everything that gets drawn on top of the frame, 
// except the time bar). It also does some sorting of events for
// the panels.
//---------------------------------------------------------------

class FrameView : public View
{
public:
	FrameView(Interface &iface) : iface(iface), drag0(-1) {}

	void draw() const override;

	bool handle_click(int x, int y, bool dbl) override;
	bool handle_scroll(int x, int y, int dy) override;

	bool  start_drag(int x, int y) override;
	void handle_drag(int x, int y) override;

private:
	Interface &iface;
	int drag0;

	bool hits_divider(int x, int y) const;
};