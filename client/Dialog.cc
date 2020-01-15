#include "Dialog.h"
#include "interface.h"
#include "client.h"

Dialog::Dialog(Interface &iface, Function f)
: iface(iface), function(f), confirming(false), cursor(0), hscroll(0)
{
	if (f == SAVE_PLIST)
	{
		str p0 = iface.client.cwd;
		if (!p0.empty() && p0.back() != '/') p0 += "/";
		p0 += ".m3u"; cursor = strwidth(p0)-4;
		response = p0;
	}
	else if (f == ADD_URL)
	{
		response = "http://";
		cursor = strwidth(response);
	}
	hscroll = cursor;
	redraw(1);
}

void Dialog::redraw(int i) { iface.redraw(i); }

bool Dialog::ok(bool confirmed)
{
	if (response.empty()) return cancel();

	switch (function)
	{
		case SAVE_PLIST:
		{
			if (!confirmed && file_exists(response)) {
				confirming = true;
				yes = false;
				redraw(2);
				return true;
			}
			iface.status("Saving the playlist...");
			iface.client.playlist.save(response);
			if (!confirmed) iface.client.handle_command(KEY_CMD_RELOAD);
			iface.status("Playlist saved.");
			break;
		}
		case ADD_URL:
		{
			iface.client.add_url(response, true);
			break;
		}
	}
	return cancel();
}

bool Dialog::cancel()
{
	iface.clear_dialog();
	return true;
}

bool Dialog::handle_key(wchar_t c, int f)
{
	if (confirming)
	{
		if (c == '\n')
		{
			if (yes) return ok(true);
			confirming = false;
			redraw(2);
			return true;
		}

		switch (f)
		{
			case KEY_LEFT:
			case KEY_RIGHT:
				yes = !yes; redraw(1); return true;
		}

		if (get_key_cmd(CON_ENTRY, c, f) == KEY_CMD_CANCEL)
		{
			confirming = false;
			redraw(2);
			return true;
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
		case KEY_END: cursor = strwidth(response); return true;

		case KEY_BACKSPACE: if (cursor) strdel(response, --cursor);  return true;
		case KEY_DC: if (cursor) strdel(response, cursor); return true;
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

void Dialog::draw() const
{
	const int W = COLS, H = LINES;
	auto &win = iface.win;

	Rect r; // bounds of the dialog
	
	if (confirming)
	{
		const char *p = "File exists. Overwrite?";
		r.w = strlen(p)+4; r.h = 5;
		r.center(W, H);

		const int W = COLS, H = LINES;
		Window &win = iface.win;

		win.moveto(r.y, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);
		win.moveto(r.y+1, r.x); win.field(p, r.w, 'C');
		win.moveto(r.y+2, r.x); win.color(CLR_MENU_ITEM); win.clear(r.w);

		win.moveto(r.y+3, r.x); win.put_ascii("  ");
		win.color(yes ? CLR_MENU_SELECTED : CLR_MENU_ITEM);
		win.field("YES", (r.w-4-2+1)/2, 'C');
		win.color(CLR_MENU_ITEM); win.put_ascii("  ");
		win.color(!yes ? CLR_MENU_SELECTED : CLR_MENU_ITEM);
		win.field("NO", (r.w-4-2)/2, 'C');
		win.color(CLR_MENU_ITEM); win.put_ascii("  ");

		win.moveto(r.y+4, r.x); win.clear(r.w);
		curs_set(0);
		return;
	}

	str ps = (function == SAVE_PLIST ? "SAVE PLAYLIST" : "ADD URL");
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
