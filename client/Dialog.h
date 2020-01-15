#pragma once
#include "View.h"
class Interface;

class Dialog : public View
{
public:
	enum Function { SAVE_PLIST, ADD_URL };
	Dialog(Interface &iface, Function f);

	void draw() const override;
	void redraw(int i);
	bool handle_key(wchar_t c, int f);

	bool start_drag(int x, int y) override { return true; }
	bool handle_click(int x, int y, bool dbl) override { return true; }
	bool handle_scroll(int x, int y, int dy) override { return true; }

private:
	Interface &iface;
	Function function;
	
	bool confirming;
	bool yes;

	str response;
	mutable int cursor, hscroll; // libreadline?

	bool ok(bool confirmed = false);
	bool cancel();
};