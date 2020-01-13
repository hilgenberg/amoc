#include "Menu.h"
#include "interface.h"

void MenuItem::execute(Interface &iface) const
{
	if (greyed()) return;
	if (execute_fn) execute_fn();
	if (cmd != KEY_CMD_WRONG) iface.handle_command(cmd);
}

Menu::Menu(Interface &iface) : iface(iface), active(false)
{
	SubMenu *m = NULL; MenuItem *mi = NULL;
	#define MENU(s) items.emplace_back(s); m = &items.back()
	#define ITEM(s,c) m->items.emplace_back(s,c); mi = &m->items.back()
	#define SEPARATOR m->items.emplace_back("",KEY_CMD_WRONG); mi = &m->items.back()
	#define ONLY_IN_DIR mi->execute_fn = [&iface](){ iface.go_to_dir_plist(); }
	#define CHK(x) mi->state_fn = [&iface]()->MenuState{ return (x) ? Checked : Unchecked; }

MENU("Files");
	ITEM("Go to music directory", KEY_CMD_GO_MUSIC_DIR);
	ITEM("Go up one directory", KEY_CMD_GO_DIR_UP); ONLY_IN_DIR;
	ITEM("Go to playing file", KEY_CMD_GO_TO_PLAYING_FILE);
	ITEM("Reload directory", KEY_CMD_RELOAD);
	SEPARATOR;
	ITEM("Add items", KEY_CMD_PLIST_ADD_FILE);
	ITEM("Add recursively", KEY_CMD_PLIST_ADD_DIR);
	SEPARATOR;
	ITEM("Quit client", KEY_CMD_QUIT_CLIENT);
	ITEM("Quit server", KEY_CMD_QUIT);

MENU("View");
	ITEM("Read tags", KEY_CMD_TOGGLE_READ_TAGS); CHK(options::ReadTags);
	ITEM("Cycle to next layout", KEY_CMD_TOGGLE_LAYOUT);
	ITEM("Hide message", KEY_CMD_HIDE_MESSAGE);
	ITEM("Show hidden files", KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES); CHK(options::ShowHiddenFiles);
	ITEM("Show full paths", KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS); CHK(options::PlaylistFullPaths);
	SEPARATOR;
	ITEM("Refresh display", KEY_CMD_REFRESH);

MENU("Playlist");
	ITEM("Move items up", KEY_CMD_PLIST_MOVE_UP);
	ITEM("Move items down", KEY_CMD_PLIST_MOVE_DOWN);
	ITEM("Clear playlist", KEY_CMD_PLIST_CLEAR);
	ITEM("Remove items", KEY_CMD_PLIST_DEL);
	ITEM("Remove dead entries", KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES);
	SEPARATOR;
	ITEM("Repeat", KEY_CMD_TOGGLE_REPEAT); CHK(options::Repeat);
	ITEM("Auto-advance songs", KEY_CMD_TOGGLE_AUTO_NEXT); CHK(options::AutoNext);
	ITEM("Shuffle", KEY_CMD_TOGGLE_SHUFFLE); CHK(options::Shuffle);
	SEPARATOR;
	ITEM("Pause", KEY_CMD_PAUSE);
	ITEM("Stop", KEY_CMD_STOP);
	ITEM("Next", KEY_CMD_NEXT);
	ITEM("Previous", KEY_CMD_PREVIOUS);
	SEPARATOR;
	ITEM("Save...", KEY_CMD_PLIST_SAVE);
	
MENU("Seek");
	ITEM("Seek forward", KEY_CMD_SEEK_FORWARD);
	ITEM("Seek backward", KEY_CMD_SEEK_BACKWARD);
	ITEM("Seek fast forward", KEY_CMD_SEEK_FORWARD_5);
	ITEM("Seek fast backward", KEY_CMD_SEEK_BACKWARD_5);
	SEPARATOR;
	ITEM("Seek to  0%", KEY_CMD_SEEK_0);
	ITEM("Seek to 10%", KEY_CMD_SEEK_1);
	ITEM("Seek to 20%", KEY_CMD_SEEK_2);
	ITEM("Seek to 30%", KEY_CMD_SEEK_3);
	ITEM("Seek to 40%", KEY_CMD_SEEK_4);
	ITEM("Seek to 50%", KEY_CMD_SEEK_5);
	ITEM("Seek to 60%", KEY_CMD_SEEK_6);
	ITEM("Seek to 70%", KEY_CMD_SEEK_7);
	ITEM("Seek to 80%", KEY_CMD_SEEK_8);
	ITEM("Seek to 90%", KEY_CMD_SEEK_9);

MENU("Rating");
	ITEM("Unrated",  KEY_CMD_RATE_0);
	ITEM("Rate 1/5", KEY_CMD_RATE_1);
	ITEM("Rate 2/5", KEY_CMD_RATE_2);
	ITEM("Rate 3/5", KEY_CMD_RATE_3);
	ITEM("Rate 4/5", KEY_CMD_RATE_4);
	ITEM("Rate 5/5", KEY_CMD_RATE_5);

MENU("Sound");
	ITEM("Equalizer", KEY_CMD_TOGGLE_EQUALIZER);
	ITEM("Refresh equalizer", KEY_CMD_EQUALIZER_REFRESH);
	ITEM("Previous equalizer", KEY_CMD_EQUALIZER_PREV);
	ITEM("Next equalizer", KEY_CMD_EQUALIZER_NEXT);
	SEPARATOR;
	ITEM("Make mono", KEY_CMD_TOGGLE_MAKE_MONO);
	SEPARATOR;
	ITEM("Decrease volume by 1%", KEY_CMD_MIXER_DEC_1);
	ITEM("Increase volume by 1%", KEY_CMD_MIXER_INC_1);
	ITEM("Decrease volume by 5%", KEY_CMD_MIXER_DEC_5);
	ITEM("Increase volume by 5%", KEY_CMD_MIXER_INC_5);
	SEPARATOR;
	ITEM("Volume 10%", KEY_CMD_VOLUME_10);
	ITEM("Volume 20%", KEY_CMD_VOLUME_20);
	ITEM("Volume 30%", KEY_CMD_VOLUME_30);
	ITEM("Volume 40%", KEY_CMD_VOLUME_40);
	ITEM("Volume 50%", KEY_CMD_VOLUME_50);
	ITEM("Volume 60%", KEY_CMD_VOLUME_60);
	ITEM("Volume 70%", KEY_CMD_VOLUME_70);
	ITEM("Volume 80%", KEY_CMD_VOLUME_80);
	ITEM("Volume 90%", KEY_CMD_VOLUME_90);
	SEPARATOR;
	ITEM("Mixer", KEY_CMD_TOGGLE_MIXER);
	ITEM("Softmixer", KEY_CMD_TOGGLE_SOFTMIXER);
}

void Menu::draw()
{
	if (!active) return;
	
	const int W = COLS, H = LINES;
	const int N = (int)items.size(); if (!N || W < 8 || H < 6) return;
	Window &win = iface.win;

	sel = CLAMP(0, sel, N-1);
	
	int tw = 0; for (auto &m : items) tw += m.title.length() + 4;
	int spc = CLAMP(0, (W-tw-4)/N, 1);

	// everything's ASCII here, so we don't bother with strwidth and the like
	win.moveto(0,0);
	win.color(CLR_MENU_ITEM); win.clear(W);
	int x0 = 0;
	for (int i = 0; i < N; ++i)
	{
		auto &m = items[i];
		if (i > 0) { win.color(CLR_MENU_ITEM); win.spaces(spc); }
		win.color(i == sel ? CLR_MENU_SELECTED : CLR_MENU_ITEM);
		win.put_ascii("  ");
		win.put_ascii(m.title);
		win.put_ascii("  ");
		if (i < sel) x0 += m.title.length() + 4 + spc;
	}

	auto &m = items[sel];
	const int n = (int)m.items.size();
	m.sel = CLAMP(0, m.sel, n-1);
	bool w0 = false; int w1 = 0, w2 = 0;
	for (auto &mi : m.items)
	{
		if (!w0 && mi.state() != Normal) w0 = true;
		w1 = std::max(w1, (int)mi.title.length());
		w2 = std::max(w2, (int)mi.key().length());
	}
	if (w2) ++w2;

	int mw = 2+w1+2 + w0*4 + w2;
	if (mw > W) { w1 = W-4-w0*4-w2; if (w1 < 4) return; }
	if (x0 + mw > W) x0 = W-mw; if (x0 < 0) x0 = 0;

	win.color(CLR_MENU_ITEM); win.moveto(1, x0); win.spaces(2+w1+w0*4+2+w2);

	for (int i = 0; i < n; ++i)
	{
		auto &mi = m.items[i];
		if (2+i >= H) break;
		win.moveto(2+i, x0);
		win.color(CLR_MENU_ITEM); win.put(' ');

		win.color(i == m.sel ? 
			mi.greyed() ? CLR_MENU_GREY_SELECTED : CLR_MENU_SELECTED :
			mi.greyed() ? CLR_MENU_GREY : CLR_MENU_ITEM);
		win.put(' ');
		if (w0) switch (mi.state())
		{
			case Normal:     win.put("    "); break;
			case Unchecked:  win.put("[ ] "); break;
			case Checked:    win.put("[X] "); break;
			case Unselected: win.put("( ) "); break;
			case Selected:   win.put("(*) "); break;
		}
		win.field(mi.title, w1);
		win.put(' ');
		if (w2) { win.put(' '); win.field(mi.key(), w2-1, 'R'); }
		
		win.color(CLR_MENU_ITEM); win.put(' ');
	}

	if (2+n < H) { win.color(CLR_MENU_ITEM); win.moveto(2+n, x0); win.spaces(2+w1+w0*4+2+w2); }
}

bool Menu::handle_command(key_cmd cmd)
{
	const int N = (int)items.size();
	sel = CLAMP(0, sel, N-1); int sel0 = sel;
	auto &m = items[sel];
	const int n = (int)m.items.size();
	m.sel = CLAMP(0, m.sel, n-1);

	// assume that there are only single separators between two non-separators
	switch (cmd)
	{
		case KEY_CMD_MENU_UP: do { m.sel += n-1; m.sel %= n; } while (m.items[m.sel].is_separator()); break;
		case KEY_CMD_MENU_DOWN: do { ++m.sel; m.sel %= n; } while (m.items[m.sel].is_separator()); break;
		case KEY_CMD_MENU_FIRST: m.sel = 0; break;
		case KEY_CMD_MENU_LAST: m.sel = n-1; break;

		case KEY_CMD_MENU_LEFT: sel += N-1; sel %= N; break;
		case KEY_CMD_MENU_RIGHT: ++sel; sel %= N; break;

		case KEY_CMD_MENU_EXEC: m.items[m.sel].execute(iface); active = false; break;
		case KEY_CMD_MENU_EXEC_NOCLOSE: m.items[m.sel].execute(iface); break;

		default: return false;
	}

	iface.redraw(sel != sel0 ? 2 : 1);

	return true;
}
