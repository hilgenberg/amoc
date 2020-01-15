#pragma once
#include "View.h"
#include "keys.h"
#include "utf8.h"
#include <functional>
class Interface;

enum MenuState
{
	Normal = 0,  //     Item Text
	Unchecked,   // [ ] Item Text
	Checked,     // [X] Item Text
	Unselected,  // ( ) Item Text
	Selected,    // (*) Item Text
};

struct MenuItem
{
	MenuItem(const char *title, key_cmd cmd) : title(title), cmd(cmd) {}
	bool is_separator() const { return title.empty(); }

	str title;
	str key() const { return sanitized(hotkey(cmd)); }
	MenuState state() const { return state_fn ? state_fn() : Normal; }
	bool greyed() const { return greyed_fn && greyed_fn(); }
	void execute(Interface &iface) const;

	key_cmd cmd;
	std::function<void(void)> execute_fn; // used before cmd, if set
	std::function<MenuState(void)> state_fn;
	std::function<bool(void)> greyed_fn;
};

struct SubMenu
{
	SubMenu(const char *title) : title(title), sel(0) {}
	str title;
	std::vector<MenuItem> items;
	mutable int sel;
};

//---------------------------------------------------------------
// Menu draws and handles the F9-menu.
//---------------------------------------------------------------

class Menu : public View
{
public:
	Menu(Interface &iface);
	bool active;
	void draw() const override;

	bool handle_key(wchar_t c, int f) override;
	bool handle_command(key_cmd cmd);
	bool handle_click(int x, int y, bool dbl) override;
	void handle_hover(int x, int y);
	bool handle_scroll(int x, int y, int dy) override { return active; }

	bool start_drag(int x, int y) override;
	void handle_drag(int x, int y) override;
	void finish_drag(int x, int y) override;

private:
	std::vector<SubMenu> items;
	mutable int sel;
	Interface &iface;
	mutable int sub_x0, sub_w, sub_spc; // position and size of active submenu
};