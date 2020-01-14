#pragma once
#include "View.h"
class Interface;

class InfoView : public View
{
public:
	InfoView(Interface &iface) : iface(iface), drag_x0(-1) {}

	void draw() const override;
	bool handle_click(int x, int y, bool dbl) override;
	bool  start_drag(int x, int y) override;
	void handle_drag(int x, int y) override;
	void finish_drag(int x, int y) override;
	bool handle_scroll(int x, int y, int dy) override;

private:
	Interface &iface;
	int drag_x0;
};