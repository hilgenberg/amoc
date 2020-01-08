#pragma once

struct Rect
{
	Rect() : x(0), y(0), w(0), h(0) {}
	Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}
	void set(int x_, int y_, int w_, int h_) { x=x_; y=y_; w=w_; h=h_; }
	int x1() const { return x+w; }
	int y1() const { return y+h; }
	Rect inset(int d) const { return Rect(x+d, y+d, w-2*d, h-2*d); }
	int x, y;
	int w, h;

	bool contains(int ex, int ey) const
	{
		return ex >= x && ey >= y && ex < x+w && ey < y+h;
	}
};
