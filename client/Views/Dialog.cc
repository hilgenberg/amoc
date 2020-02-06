#include "Dialog.h"
#include "../interface.h"
#include "../client.h"

static void trim(str &s, bool remove_track_number=false)
{
	int i0 = 0, i1 = (int)s.length() - 1;
	while (i0 <= i1 && (isspace(s[i0]) || s[i0] == '-' || s[i0] == '/')) ++i0;
	while (i0 <= i1 && (isspace(s[i1]) || s[i1] == '-' || s[i1] == '/')) --i1;
	if (i0 > i1) s.clear(); else s = s.substr(i0, i1-i0+1);
	i0 = 0; i1 = (int)s.length() - 1;
	if (!remove_track_number) return;

	while (i0 <= i1 && isdigit(s[i0])) ++i0;
	if (i0 <= i1 && (s[i0] == ' ' || s[i0] == '-'))
	{
		++i0;
		s = s.substr(i0+1);
		trim(s, false);
	}
}
static file_tags default_tags(Client &client, const plist &pl, const int i0, const int i1)
{
	if (i0 < 0 || i1 < i0) throw std::logic_error("Trying to edit tags for empty selection");

	file_tags tags;

	// if they share common tags, start with that
	for (int i = i0; i <= i1; ++i)
	{
		str t = client.get_artist(pl[i]);
		if (tags.artist.empty()) tags.artist = t;
		else if (tags.artist != t) { tags.artist.clear(); break; }
	}
	for (int i = i0; i <= i1; ++i)
	{
		str t = client.get_album(pl[i]);
		if (tags.album.empty()) tags.album = t;
		else if (tags.album != t) { tags.album.clear(); break; }
	}
	if (i0 == i1)
	{
		tags.title = client.get_title(pl[i0]);
		tags.track = client.get_track(pl[i0]);
	}
	if (!tags.artist.empty() && !tags.album.empty() && !tags.title.empty()) return tags;

	// otherwise use longest common path prefix
	str path = pl[i0].path;
	for (int i = i0+1; i <= i1; ++i)
	{
		intersect(path, pl[i].path);
		if (path.length() <= 1) break;
	}
	if (path.length() <= 1) return tags;

	str mhome = options::MusicDir; if (!mhome.empty()) mhome += '/'; if (mhome.length() < 2) mhome.clear();
	str uhome = options::Home;     if (!uhome.empty()) uhome += '/'; if (uhome.length() < 2) uhome.clear();
	if      (!mhome.empty() && has_prefix(path, mhome, false)) path = path.substr(mhome.length());
	else if (!uhome.empty() && has_prefix(path, uhome, false)) path = path.substr(uhome.length());
	
	// remove final / (when i0 != i1)
	if (!path.empty() && path.back() == '/') path.pop_back();

	// remove extension (when i0 == i1)
	const char *s0 = path.c_str(), *s = ext_pos(s0);
	if (s) path = path.substr(0, s-1-s0);

	// remove set tags from path
	if (!tags.artist.empty()) while (true)
	{
		size_t k = path.find(tags.artist); if (k == str::npos) break;
		path.erase(k, tags.artist.length());
	}
	if (!tags.album.empty()) while (true)
	{
		size_t k = path.find(tags.album); if (k == str::npos) break;
		path.erase(k, tags.album.length());
	}
	if (!tags.title.empty()) while (true)
	{
		size_t k = path.find(tags.title); if (k == str::npos) break;
		path.erase(k, tags.title.length());
	}
	while (true)
	{
		size_t k = path.find("//"); if (k == str::npos) break;
		path.erase(k, 1);
	}
	if (!path.empty() && path[0] == '/') path.erase(0, 1);

	// only keep two directory levels
	size_t k = path.rfind('/');
	if (k != str::npos) k = k > 0 ? path.rfind('/', k-1) : str::npos;
	if (k != str::npos) k = k > 0 ? path.rfind('/', k-1) : str::npos;
	if (k != str::npos) path.erase(0, k+1);

	if (tags.artist.empty()) { tags.artist = path; trim(tags.artist); }
	if (tags.album.empty()) { tags.album = path; trim(tags.album); }

	if (tags.title.empty())
	{
		k = path.rfind('/');
		if (k != str::npos) path.erase(0, k+1);

		tags.title = path;
		trim(tags.title, true);
	}

	return tags;
}
static str default_dst(const plist &pl, const int i0, const int i1)
{
	if (i0 < 0 || i1 < i0) throw std::logic_error("Trying to move empty set of files");

	// if they share common tags, start with that
	str ret;
	bool first = true;
	for (int i = i0; i <= i1; ++i)
	{
		auto &it = pl[i];
		if (it.type ==  F_URL) continue;
		if (first) ret = it.path; else intersect(ret, it.path);
		first = false;
	}
	auto i = ret.rfind('/');
	if (i != str::npos)
	{
		if (i0 == i1)
			ret = ret.substr(i+1);
		else
			if (i != 0) ret = ret.substr(0, i+1);
	}
	return ret;
}

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
			auto tag = default_tags(iface.client, iface.active->items, sel.first, sel.second);

			response = (function==EDIT_ARTIST ? tag.artist :
				function==EDIT_ALBUM ? tag.album : tag.title);
			break;
		}

		case CONFIRM_QUIT:
		case CONFIRM_QUIT_CLIENT:
			confirming = format(" Discard %d tag%s? ", 
				(int)iface.client.tags.changes.size(),
				iface.client.tags.changes.size()==1 ? "" : "s");
			break;

		case FILES_MV:
		{
			auto sel = iface.selection();
			response = default_dst(iface.active->items, sel.first, sel.second);
			break;
		}

		case FILES_RM:
		{
			auto sel = iface.selection();
			confirming = format("Really delete %d items?", sel.second-sel.first+1);
			break;
		}
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
				(iface.client.*mf)(pl[i], response);

			iface.redraw(3);

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

		case FILES_MV: iface.client.files_mv(response); break;
		case FILES_RM: assert(confirmed); iface.client.files_rm(); break;
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
			if (function == CONFIRM_QUIT || function == CONFIRM_QUIT_CLIENT ||
			    function == FILES_RM)
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

		default: break;
	}

	switch (get_key_cmd(CON_ENTRY, c, f))
	{
		case KEY_CMD_CANCEL: return cancel();
		case KEY_CMD_HISTORY_UP: /*TODO*/ return true;
		case KEY_CMD_HISTORY_DOWN: /*TODO*/ return true;
		case KEY_CMD_DELETE_START: strdel(response, 0, cursor); cursor = 0; return true;
		case KEY_CMD_DELETE_END: strdel(response, cursor, strwidth(response)); return true;
		default: break;
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
		case Dialog::FILES_MV:    return "DESTINATION";
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
		win.field(sanitized(strtail(response, n-hscroll-3)), w-3);
	} else {
		win.field(sanitized(response), w);
	}
	win.color(CLR_MENU_ITEM); win.put_ascii("  ");

	win.moveto(r.y+4, r.x); win.clear(r.w);

	// no more drawing after this!
	win.moveto(r.y+3, r.x+2 + cursor-hscroll);
	curs_set(2);
}
