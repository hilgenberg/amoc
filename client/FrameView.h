#pragma once
#include "View.h"
class Interface;

class FrameView : public View
{
public:
	FrameView(Interface &iface) : iface(iface), drag0(-1) {}

	void draw() const override;

	bool handle_click(int x, int y, bool dbl) override;
	bool handle_scroll(int x, int y, int dy) override;

	bool  start_drag(int x, int y) override;
	void handle_drag(int x, int y) override;
	void finish_drag(int x, int y) override;

private:
	Interface &iface;
	int drag0;

	bool hits_divider(int x, int y) const;
};