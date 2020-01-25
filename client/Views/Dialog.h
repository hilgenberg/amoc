#pragma once
#include "View.h"
class Interface;

class Dialog : public View
{
public:
	enum Function
	{
		SAVE_PLIST,
		ADD_URL,
		EDIT_ARTIST, EDIT_ALBUM, EDIT_TITLE,
		FILES_MV, FILES_RM,
		CONFIRM_QUIT, CONFIRM_QUIT_CLIENT
	};
	
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

	mutable int cursor, hscroll;
	str response;
	
	str confirming; bool yes;

	bool ok(bool confirmed = false);
	bool cancel();
};