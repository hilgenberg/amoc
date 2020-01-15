#pragma once
#include "View.h"
#include "../server/server.h" // PlayState

class Interface;


//---------------------------------------------------------------
// InfoView draws the bottom part of the screen, which is mostly
// information on the current song.
//---------------------------------------------------------------

class InfoView : public View
{
public:
	InfoView(Interface &iface);

	void draw() const override;
	void redraw(int i);

	bool handle_click(int x, int y, bool dbl) override;
	bool  start_drag(int x, int y) override;
	void handle_drag(int x, int y) override;
	void finish_drag(int x, int y) override;
	bool handle_scroll(int x, int y, int dy) override;

	#define UPD(T,x)\
		void update_ ## x(T v) { if (x==v) return; x=v; redraw(1); } \
		T get_ ## x() const { return x; }
	UPD(int, bitrate)
	UPD(int, avg_bitrate)
	UPD(int, rate)
	UPD(int, curr_time)
	UPD(int, channels)
	UPD(PlayState, state)
	UPD(str, mixer_name)
	UPD(int, mixer_value)
	#undef UPD

private:
	Interface &iface;
	int drag_x0;

	mutable int bitrate, avg_bitrate; // in kbps
	mutable int rate;		  // in kHz
	mutable int curr_time;
	mutable int channels;
	mutable PlayState state; // STATE_(PLAY | STOP | PAUSE)
	mutable str mixer_name;
	mutable int mixer_value;
};