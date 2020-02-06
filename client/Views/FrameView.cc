#include "FrameView.h"
#include "../interface.h"
#include "../client.h"

#define CURR_RATIO (options::layout == HSPLIT ? options::hsplit : options::vsplit)

bool FrameView::hits_divider(int x, int y) const
{
	return options::layout != SINGLE &&
		!iface.left.bounds.contains(x,y) && 
		!iface.right.bounds.contains(x,y) &&
		(iface.left.bounds+iface.right.bounds).contains(x,y);
}

bool FrameView::handle_click(int x, int y, bool dbl)
{
	if (dbl && hits_divider(x, y))
	{
		CURR_RATIO = std::make_pair(1,1);
		iface.redraw(3);
		return true;
	}

	auto &left = iface.left, &right = iface.right, *&active = iface.active;

	if (options::layout != SINGLE && left.bounds.contains(x,y))
	{
		if (active == &right) { active = &left; iface.redraw(2); }
		return left.handle_click(x, y, dbl);
	}
	else if (options::layout != SINGLE && right.bounds.contains(x,y))
	{
		if (active == &left) { active = &right; iface.redraw(2); }
		return right.handle_click(x, y, dbl);
	}
	else if (options::layout == SINGLE && active->bounds.contains(x,y))
	{
		return active->handle_click(x, y, dbl);
	}

	return false;
}

bool FrameView::handle_scroll(int x, int y, int dy)
{
	auto &left = iface.left, &right = iface.right, *&active = iface.active;
	Panel *m = NULL;
	if (options::layout != SINGLE && left.bounds.contains(x,y)) m = &left;
	else if (options::layout != SINGLE && right.bounds.contains(x,y)) m = &right;
	else if (options::layout == SINGLE && active->bounds.contains(x,y)) m = active;
	if (m)
	{
		active = m;
		while (dy < 0) { m->move_selection(REQ_SCROLL_UP);   ++dy; }
		while (dy > 0) { m->move_selection(REQ_SCROLL_DOWN); --dy; }
		iface.redraw(2);
		return true;
	}

	return false;
}

bool FrameView::start_drag(int x, int y)
{
	drag0 = -1;

	if (!hits_divider(x, y)) return handle_click(x, y, false);

	if (options::layout == HSPLIT)
	{
		drag0 = x;
		options::hsplit = std::make_pair(iface.right.bounds.w, iface.left.bounds.w);
	}
	else
	{
		drag0 = y;
		options::vsplit = std::make_pair(iface.right.bounds.h, iface.left.bounds.h);
	}

	return true;
}

void FrameView::handle_drag(int x, int y)
{
	if (drag0 < 0) return;

	auto &r = CURR_RATIO;
	int p = (options::layout == HSPLIT ?  x :  y);
	int s = (options::layout == HSPLIT ? +1 : -1);

	if (p != drag0)
	{
		r.first  -= s*(p-drag0);
		r.second += s*(p-drag0);
		if (r.first  < 0) { r.second -= r.first;  r.first  = 0; }
		if (r.second < 0) { r.first  -= r.second; r.second = 0; }
		drag0 = p;
		iface.redraw(3);
	}
}

void FrameView::draw() const
{
	auto &left = iface.left, &right = iface.right, *active = iface.active;
	auto &client = iface.client;
	auto &win = iface.win;
	Rect r1 = left.bounds.inset(-1), r2 = right.bounds.inset(-1);

	const int W = COLS, H = LINES;

	str mhome = options::MusicDir; if (!mhome.empty()) mhome += '/'; if (mhome.length() < 2) mhome.clear();
	str uhome = options::Home;     if (!uhome.empty()) uhome += '/'; if (uhome.length() < 2) uhome.clear();

	if (options::layout != SINGLE || active == &left)
	{
		str s = client.cwd; normalize_path(s);

		if (!mhome.empty() && has_prefix(s, mhome, false))
		{
			s = s.substr(mhome.length());
		}
		else if (!uhome.empty() && has_prefix(s, uhome, false))
		{
			assert(uhome.length() >= 3);
			s = s.substr(uhome.length()-2);
			s[0] = '~'; s[1] = '/';
		}
		else if (s+"/" == uhome) s = "~";
		win.sanitize_path(s);
		win.frame(r1, s, options::layout == VSPLIT ? 14 : 0, false);
	}
	if (options::layout != SINGLE || active == &right)
		win.frame(r2, client.synced ? "Playlist" : "Playlist (local)", 0, false);

	// bottom frame
	win.color(CLR_FRAME);
	win.put(H-4, 0, win.ltee);
	win.hl(W-2);
	win.put(H-4, W-1, win.rtee);
	if (options::layout == HSPLIT)
	{
		win.put(H-4, left.bounds.x1(), win.lrcorn);
		win.put(win.llcorn);
	}
	else if (options::layout == VSPLIT)
	{
		int y = r2.y1()-1;
		win.put(y,   0, win.ltee);
		win.put(y, W-1, win.rtee);
	}

	// sides and corners
	win.put(H-3, 0,   win.vert);
	win.put(H-2, 0,   win.vert);
	win.put(H-3, W-1, win.vert);
	win.put(H-2, W-1, win.vert);
	win.put(H-1, 0,   win.llcorn);
	win.put(H-1, W-1, win.lrcorn);

	// playlist total times
	if (left.bounds.w >= 30 && (options::layout != SINGLE || active == &left))
	{
		int x1 = left.bounds.x1()-1, x0 = x1 - Window::TOTAL_TIME_WIDTH - 1;
		int y = left.bounds.y1();
		
		win.color(CLR_PLIST_TIME);
		win.moveto(y, x0+1);
		win.total_time(iface.left_total);

		win.color(CLR_FRAME);
		win.put(y, x0, '[');
		win.put(y, x1, ']');
	}
	if (right.bounds.w >= 30 && (options::layout != SINGLE || active == &right))
	{
		int x1 = right.bounds.x1()-1, x0 = x1 - Window::TOTAL_TIME_WIDTH - 1;
		int y = right.bounds.y1();

		win.color(CLR_PLIST_TIME);
		win.moveto(y, x0+1);
		win.total_time(iface.right_total);

		win.color(CLR_FRAME);
		win.put(y, x0, '[');
		win.put(y, x1, ']');
	}

	// status message
	if (!iface.status_msg.empty())
	{
		int w = 0, sw = (int)iface.status_msg.length();

		if (options::layout == HSPLIT)
		{
			if (left.bounds.w >= 30) w = left.bounds.w - 15;
			if (w < sw) w = left.bounds.w - 2;
			if (w < sw && right.bounds.w >= 30) w = W - 17;
		}
		else
		{
			if (w < sw && W >= 32) w = W - 17;
		}
	
		if (w < sw) w = W - 4;
		if (w < sw) sw = w;

		win.color(CLR_FRAME);
		win.moveto(H-4, 1);
		win.hl(w+2); // overwrite playlist times if needed
		int x0 = 1+(w-sw)/2;
		win.put(H-4, x0, ' '); //win.rtee);
		win.put(H-4, x0+sw+1, ' '); //win.ltee);

		win.color(CLR_STATUS);
		win.moveto(H-4, x0+1);
		win.field(iface.status_msg, sw);
	}

}