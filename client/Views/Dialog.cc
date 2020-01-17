#include "Dialog.h"
#include "../interface.h"
#include "../client.h"

#define TAG(i) (\
	function==EDIT_ARTIST ? iface.client.get_artist(pl[i]) : \
	function==EDIT_ALBUM  ? iface.client.get_album (pl[i]) : \
	                        iface.client.get_title (pl[i]))
#define PATH(i) (pl[i].path)

Dialog::Dialog(Interface &iface, Function f)
: iface(iface), function(f), cursor(-1), yes(false)
{
	switch (function)
	{
		case SAVE_PLIST:
			response = iface.client.cwd;
			if (!response.empty() && response.back() != '/') response += "/";
			response += ".m3u"; 
			cursor = strwidth(response)-4;
			break;
		
		case ADD_URL:
			response = "http://";
			break;

		case EDIT_ARTIST:
		case EDIT_ALBUM:
		case EDIT_TITLE:
		{
			auto sel = iface.selection();
			const int n = (sel.second+1-sel.first);
			if (sel.first < 0 || n <= 0) throw std::logic_error("Trying to edit tags for empty selection");
			auto &pl = iface.active->items;

			// if they share a common tag, start with that
			for (int i = sel.first; i <= sel.second; ++i)
			{
				str t = TAG(i);
				if (response.empty())
					response = t;
				else if (response != t)
				{
					response.clear();
					break;
				}
			}

			// otherwise use longest common path prefix
			if (response.empty())
			{
				str mhome = options::MusicDir; if (!mhome.empty()) mhome += '/'; if (mhome.length() < 2) mhome.clear();
				str uhome = options::Home;     if (!uhome.empty()) uhome += '/'; if (uhome.length() < 2) uhome.clear();

				for (int i = sel.first; i <= sel.second; ++i)
				{
					str t = PATH(i);
					if (response.empty())
						response = t;
					else
					{
						intersect(response, t);
						if (response.empty()) break;
					}
				}

				if (!mhome.empty() && has_prefix(response, mhome, false))
					response = response.substr(mhome.length());
				else if (!uhome.empty() && has_prefix(response, uhome, false))
					response = response.substr(uhome.length());
				if (!response.empty() && response.back() == '/')
					response.pop_back();
	
				const char *s0 = response.c_str(), *s = ext_pos(s0);
				if (s) response = response.substr(0, s-1-s0);
			}
			break;
		}

		case CONFIRM_QUIT:
		case CONFIRM_QUIT_CLIENT:
			confirming = format(" Discard %d tag%s? ", 
				(int)iface.client.tag_changes.size(),
				iface.client.tag_changes.size()==1 ? "" : "s");
			break;
	}
	if (cursor < 0) cursor = strwidth(response);
	hscroll = cursor; // if needed, cut off the front rather than the end

	redraw(2);
}

void Dialog::redraw(int i) { iface.redraw(i); }

bool Dialog::cancel()
{
	iface.clear_dialog();
	return true;
}

bool Dialog::ok(bool confirmed)
{
	switch (function)
	{
		case SAVE_PLIST:
			if (response.empty()) return cancel();

			if (!confirmed && file_exists(response)) {
				confirming = "File exists. Overwrite?";
				yes = false;
				redraw(2);
				return true;
			}
			iface.client.playlist.save(response);
			if (!confirmed) iface.client.handle_command(KEY_CMD_RELOAD);
			iface.message("Playlist saved.");
			break;
		
		case ADD_URL:
			if (response.empty()) return cancel();
			iface.client.add_url(response, true);
			break;
		
		case EDIT_ARTIST:
		case EDIT_ALBUM:
		case EDIT_TITLE:
		{
			auto sel = iface.selection(); if (sel.first < 0) return cancel();
			auto &pl = iface.active->items;

			auto mf = (function==EDIT_ARTIST ? &Client::change_artist :
			           function==EDIT_ALBUM  ? &Client::change_album  :
				   &Client::change_title);

			for (int i = sel.first; i <= sel.second; ++i)
				(iface.client.*mf)(PATH(i), response);

			iface.redraw(2);

			break;
		}
		case CONFIRM_QUIT:
			assert(confirmed);
			iface.client.want_quit = 2;
			break;
		case CONFIRM_QUIT_CLIENT:
			assert(confirmed);
			iface.client.want_quit = 1;
			break;
	}
	return cancel();
}

bool Dialog::handle_key(wchar_t c, int f)
{
	if (!confirming.empty())
	{
		if (c == 'y' || c == 'Y' || (c == '\n' &&  yes)) return ok(true);
		if (c == 'n' || c == 'N' || (c == '\n' && !yes) ||
		    get_key_cmd(CON_ENTRY, c, f) == KEY_CMD_CANCEL)
		{
			if (function == CONFIRM_QUIT || function == CONFIRM_QUIT_CLIENT)
				return cancel();
			
			confirming.clear();
			redraw(2);
		}
		else if (c == '\t' || f == KEY_LEFT || f == KEY_RIGHT)
		{
			yes = !yes;
			redraw(1);
		}
		return true;
	}
	
	redraw(1);

	if (c == '\n')
	{
		return ok();
	}
	else if (c)
	{
		cursor += strins(response, cursor, c);
		return true;
	}

	switch (f)
	{
		case KEY_LEFT:  --cursor; return true;
		case KEY_RIGHT: ++cursor; return true;
		case KEY_HOME: cursor = 0; return true;
		case KEY_END:  cursor = strwidth(response); return true;

		case KEY_BACKSPACE: if (cursor) strdel(response, --cursor);  return true;
		case KEY_DC: strdel(response, cursor); return true;
	}

	switch (get_key_cmd(CON_ENTRY, c, f))
	{
		case KEY_CMD_CANCEL: return cancel();
		case KEY_CMD_HISTORY_UP: /*TODO*/ return true;
		case KEY_CMD_HISTORY_DOWN: /*TODO*/ return true;
		case KEY_CMD_DELETE_START: strdel(response, 0, cursor); cursor = 0; return true;
		case KEY_CMD_DELETE_END: strdel(response, cursor, strwidth(response)); return true;
	}
	return true;
}

static str prompt(Dialog::Function f)
{
	switch (f)
	{
		case Dialog::SAVE_PLIST:  return "SAVE PLAYLIST";
		case Dialog::ADD_URL:     return "ADD URL";
		case Dialog::EDIT_ARTIST: return "ARTIST";
		case Dialog::EDIT_ALBUM:  return "ALBUM";
		case Dialog::EDIT_TITLE:  return "TITLE";
		default: assert(false); return "???";
	}
}

void Dialog::draw() const
{
	const int W = COLS, H = LINES;
	auto &win = iface.win;

	Rect r; // bounds of the dialog
	
	if (!confirming.empty())
	{
		r.w = confirming.length()+4; r.h = 5;
		r.center(W, H);

		const int W = COLS, H = LINES;
		Window &win = iface.win;

		win.moveto(r.y, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);
		win.moveto(r.y+1, r.x); win.field(confirming, r.w, 'C');
		win.moveto(r.y+2, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);

		int bspc = (r.w-5-4-2)/4, spc = r.w-5-4-4*bspc;
		win.moveto(r.y+3, r.x); win.put_ascii("  ");
		win.color(yes ? CLR_MENU_SELECTED : CLR_MENU_ITEM);
		win.field("YES", 3+2*bspc, 'C');
		win.color(CLR_MENU_ITEM); win.spaces(spc);
		win.color(!yes ? CLR_MENU_SELECTED : CLR_MENU_ITEM);
		win.field("NO", 2+2*bspc, 'C');
		win.color(CLR_MENU_ITEM); win.put_ascii("  ");

		win.moveto(r.y+4, r.x); win.clear(r.w);
		curs_set(0);
		return;
	}

	str ps = prompt(function);
	r.w = CLAMP(ps.length()+4, W*3/4, std::min(80, W)); r.h = 5;
	r.center(W, H);

	int w = r.w-4;
	int n = strwidth(response);
	
	cursor = CLAMP(0, cursor, n);
	hscroll = CLAMP(0, hscroll, cursor);
	int nn = n + (cursor==n);
	if (w >= nn || hscroll < 0) hscroll = 0;

	int la = std::min(5, w/3);
	if (cursor-hscroll < la) hscroll = std::max(0, cursor-la);
	if (hscroll+w-cursor < la) hscroll = std::max(0, la+cursor-w);
	if (hscroll > 0 && hscroll+w > nn) hscroll = std::max(0, nn-w);

	win.moveto(r.y, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);
	win.moveto(r.y+1, r.x); win.field(ps, r.w, 'C');
	win.moveto(r.y+2, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);

	win.moveto(r.y+3, r.x); win.put_ascii("  ");
	win.color(CLR_ENTRY);
	if (hscroll > 0) {
		win.put_ascii("...");
		win.field(xstrtail(response, n-hscroll-3), w-3);
	} else {
		win.field(response, w);
	}
	win.color(CLR_MENU_ITEM); win.put_ascii("  ");

	win.moveto(r.y+4, r.x); win.clear(r.w);

	// no more drawing after this!
	win.moveto(r.y+3, r.x+2 + cursor-hscroll);
	curs_set(2);
}
