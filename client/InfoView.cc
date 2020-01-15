#include "InfoView.h"
#include "interface.h"
#include "Window.h"
#include "keys.h"
#include "../server/server.h" // PlayState

#include <locale.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/select.h>

#include "client.h"
#include "utf8.h"
#include "themes.h"
#include "keys.h"
#include "../playlist.h"
#include "../protocol.h"
#include "../rcc.h"
#include "../server/input/decoder.h"
#include "../server/output/softmixer.h"
#include "../server/ratings.h"

// width of the toggles for shuffle, repeat, ...
static constexpr int w_toggles = 6+3+7+6+4+5*2+4*1;

InfoView::InfoView(Interface &iface)
: iface(iface), drag_x0(-1)
, bitrate(-1), avg_bitrate(-1), rate(-1)
, curr_time(0), channels(0)
, state(STATE_STOP), mixer_value(-1)
{
}

void InfoView::redraw(int i) { iface.redraw(i); }

bool InfoView::handle_click(int x, int y, bool dbl)
{
	const int W = COLS, H = LINES;
	const int total_time = iface.get_total_time();
	auto &client = iface.client;

	// time bar --> seek
	if (y == H-1 && W > 8)
	{
		if (total_time <= 0 || options::TimeBarLine.empty()) return false;
		client.jump_to(x*total_time/(W-1));
		return true;
	}

	// play/pause state --> toggle play/pause
	if (y == H-3 && x > 0 && x < 4)
	{
		client.handle_command(KEY_CMD_PAUSE);
		return true;
	}

	// file --> show file in dir list
	if (dbl && y == H-3 && x >= 4 && x < W-1)
	{
		iface.go_to_dir_plist();
		client.handle_command(KEY_CMD_GO_TO_PLAYING_FILE);
		return true;
	}

	if (y == H-2 && W >= w_toggles+2 && x >= W-w_toggles-1 && x <= W-2)
	{
		x -= W-w_toggles-1;

		#define CHK(len, cmd) \
			if (x >= 0 && x < (len)+2) {\
				client.handle_command(cmd);\
				return true;\
			}\
			x -= (len)+3

		CHK(6,KEY_CMD_TOGGLE_MAKE_MONO); // STEREO
		x -= 3+3; // NET
		CHK(7,KEY_CMD_TOGGLE_SHUFFLE); // SHUFFLE
		CHK(6,KEY_CMD_TOGGLE_REPEAT); // REPEAT
		CHK(4,KEY_CMD_TOGGLE_AUTO_NEXT); // NEXT
		#undef CHK
		return true;
	}

	return false;
}

bool InfoView::start_drag(int x, int y)
{
	const int W = COLS, H = LINES;

	if (y < H-3) return false;

	if (y == H-1)
	{
		drag_x0 = x;
		return handle_click(x, y, false);
	}

	drag_x0 = -1;
	return true;
}

void InfoView::handle_drag(int x, int y)
{
	if (drag_x0 == -1 || drag_x0 == x) return;
	handle_click(x, LINES-1, false);
	drag_x0 = x;
}

void InfoView::finish_drag(int x, int y)
{
	if (drag_x0 == -1 && y != LINES-1) handle_click(x, y, false);
}

bool InfoView::handle_scroll(int x, int y, int dy)
{
	const int W = COLS, H = LINES;
	const int total_time = iface.get_total_time();

	// time bar --> seek
	if (y == H-1 && x >= 1 && x <= W-2 && W > 8)
	{
		if (total_time <= 0 || options::TimeBarLine.empty()) return false;
		iface.client.handle_command(dy < 0 ? KEY_CMD_SEEK_BACKWARD : KEY_CMD_SEEK_FORWARD);
		return true;
	}

	return false;
}

void InfoView::draw() const
{
	const int W = COLS, H = LINES;
	const int total_time = iface.get_total_time();
	auto &win = iface.win;

	if (state == STATE_STOP)
	{
		bitrate = avg_bitrate = rate = 0;
		curr_time = 0;
		channels = 0;
	}

	// current song: play/pause state
	win.color(CLR_BACKGROUND);
	win.moveto(H-3, 1); win.clear(W-2);
	win.color(CLR_STATE);
	win.moveto(H-3, 1);
	switch (state) {
		case STATE_PLAY:  win.put_ascii(" > "); break;
		case STATE_STOP:  win.put_ascii("[] "); break;
		case STATE_PAUSE: win.put_ascii("|| "); break;
		default: win.put_ascii("BUG"); break;
	}

	// current song title or message
	if (!iface.messages.empty())
	{
		str msg = iface.messages.front();
		win.color(has_prefix(msg, "ERROR", true) ? CLR_ERROR : CLR_MESSAGE);
		win.field(msg, W-5);
	}
	else
	{
		str mhome = options::MusicDir; if (!mhome.empty()) mhome += '/'; if (mhome.length() < 2) mhome.clear();
		str uhome = options::Home;     if (!uhome.empty()) uhome += '/'; if (uhome.length() < 2) uhome.clear();

		win.color(CLR_TITLE);

		str s = iface.curr_file;
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
		sanitize(s);
		if (options::FileNamesIconv) s = files_iconv_str (s);
		if (options::UseRCCForFilesystem) s = rcc_reencode(s);

		win.field(s, W-6, 'l');
	}

	// time bar
	str c1 = options::TimeBarLine, c2 = options::TimeBarSpace;
	if (total_time <= 0 || c1.empty())
	{
		win.moveto(H-1, 1);
		win.color(CLR_FRAME);
		win.hl(W-2);
	}
	else
	{
		int l1 = std::max(0, std::min(W-2, (W-2)*curr_time/total_time));
		if (c1.empty()) c1 = win.horiz;
		if (c2.empty()) c2 = c1;
		win.moveto(H-1, 1);
		win.color(CLR_TIME_BAR_FILL);
		for (int x = 0; x < l1; ++x) win.put(c1);
		win.color(CLR_TIME_BAR_EMPTY);
		for (int x = l1; x < W-2; ++x) win.put(c2);
	}

	win.moveto(H-2, 1);
	win.color(CLR_BACKGROUND);
	win.clear(W-2);

	int w = W-2, x = 1;

	// toggles
	if (w >= w_toggles)
	{
		win.moveto(H-2, x+w-w_toggles);
		#define SW(x, v) win.color((v) ? CLR_INFO_ENABLED : CLR_INFO_DISABLED);\
			win.put_ascii("[" #x "]")
		SW(STEREO, channels==2); win.put(' ');
		SW(NET, is_url(iface.curr_file.c_str())); win.put(' ');
		SW(SHUFFLE, options::Shuffle); win.put(' ');
		SW(REPEAT, options::Repeat); win.put(' ');
		SW(NEXT, options::AutoNext);
		#undef SW
		w -= w_toggles + 1;
	}

	// time for current song
	constexpr int TW = Window::TIME_WIDTH;
	win.color(CLR_TIME_CURRENT);
	if (w >= 5)
	{
		win.moveto(H-2, x);
		win.time(curr_time);
		x += TW+1; // time + one space
		w -= TW+1;
	}
	if (w >= 13)
	{
		win.moveto(H-2, x); win.time(total_time - curr_time);
		win.moveto(H-2, x+TW+2); win.time(total_time);
		win.color(CLR_TIME_TOTAL_FRAMES);
		win.put(H-2, x+TW+1,   '[');
		win.put(H-2, x+2*TW+2, ']');
		x += 2*TW+4; // printed stuff + one space
		w -= 2*TW+4;
	}
	++x; --w; // two spaces to whatever comes next

	// rate, bitrate, volume
	if (w >= 15)
	{
		win.moveto(H-2, x);
		win.color(CLR_LEGEND);
		win.put_ascii("   kHz     kbps");		

		win.color(CLR_SOUND_PARAMS);
		win.moveto(H-2, x);
		if (rate >= 0) win.put_ascii(format("%3d", rate)); else win.put_ascii("   ");
		win.moveto(H-2, x+7);
		if (bitrate >= 0) win.put_ascii(format("%4d", std::min(bitrate, 9999))); else win.put_ascii("    ");
		x += 15+2;
		w -= 15+2;
	}

	if (options::ShowMixer)
	{
		str ms = format("%s: %02d%%", mixer_name.c_str(), mixer_value);
		if (w >= ms.length())
		{
			win.color(CLR_SOUND_PARAMS);
			win.moveto(H-2, x);
			win.put(ms);
			//x += ms.length()+2; w -= ms.length()+2
		}
	}
}
