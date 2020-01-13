#pragma once
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
	int sel;
};

class Menu
{
public:
	Menu(Interface &iface);
	bool active;
	void draw();

	bool handle_command(key_cmd cmd);
	void handle_click(int x, int y);
	void handle_hover(int x, int y);

private:
	std::vector<SubMenu> items;
	int sel;
	Interface &iface;
	int sub_x0, sub_w, sub_spc; // position and size of active submenu
};