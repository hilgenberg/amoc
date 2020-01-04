#pragma once

#include "playlist.h"

/* Definition of events sent by server to the client. */
#define EV_STATE	0x01 /* server has changed the play/pause/stopped state */
#define EV_CTIME	0x02 /* current time of the song has changed */
#define EV_SRV_ERROR	0x04 /* an error occurred */
#define EV_BUSY		0x05 /* too many clients are connected to the server */
#define EV_DATA		0x06 /* data in response to a request follows */
#define EV_BITRATE	0x07 /* the bitrate has changed */
#define EV_RATE		0x08 /* the rate has changed */
#define EV_CHANNELS	0x09 /* the number of channels has changed */
#define EV_EXIT		0x0a /* the server is about to exit */
#define EV_PONG		0x0b /* response for CMD_PING */
#define EV_OPTIONS	0x0c /* the options (repeat, shuffle, autonext) have changed */
#define EV_SEND_PLIST	0x0d /* request for sending the playlist */
#define EV_TAGS		0x0e /* tags for the current file have changed */
#define EV_STATUS_MSG	0x0f /* followed by a status message */
#define EV_MIXER_CHANGE	0x10 /* the mixer channel was changed */
#define EV_FILE_TAGS	0x11 /* tags in a response for tags request */
#define EV_AVG_BITRATE  0x12 /* average bitrate has changed (new song) */
#define EV_AUDIO_START	0x13 /* playing of audio has started */
#define EV_AUDIO_STOP	0x14 /* playing of audio has stopped */
#define EV_FILE_RATING	0x15 /* rantings changed for a file */

/* Events caused by a client that wants to modify the playlist (see
 * CMD_CLI_PLIST* commands). */
#define EV_PLIST_ADD	0x50 /* add an item, followed by the file name */
#define EV_PLIST_DEL	0x51 /* delete an item, followed by the file name */
#define EV_PLIST_MOVE	0x52 /* move an item, followed by 2 file names */
#define EV_PLIST_CLEAR	0x53 /* replace the playlist */

/* Definition of server commands. */
#define CMD_PLAY	0x00 /* play the first element on the list */
#define CMD_LIST_CLEAR	0x01 /* clear the list */
#define CMD_LIST_ADD	0x02 /* add an item to the list */
#define CMD_STOP	0x04 /* stop playing */
#define CMD_PAUSE	0x05 /* pause */
#define CMD_UNPAUSE	0x06 /* unpause */
#define CMD_GET_CTIME	0x0d /* get the current song time */
#define CMD_GET_SNAME	0x0f /* get the stream file name */
#define CMD_NEXT	0x10 /* start playing next song if available */
#define CMD_QUIT	0x11 /* shutdown the server */
#define CMD_SEEK	0x12 /* seek in the current stream */
#define CMD_GET_STATE	0x13 /* get the state */
#define CMD_DISCONNECT	0x15 /* disconnect from the server */
#define CMD_GET_BITRATE	0x16 /* get the bitrate */
#define CMD_GET_RATE	0x17 /* get the rate */
#define CMD_GET_CHANNELS	0x18 /* get the number of channels */
#define CMD_PING	0x19 /* request for EV_PONG */
#define CMD_GET_MIXER	0x1a /* get the volume level */
#define CMD_SET_MIXER	0x1b /* set the volume level */
#define CMD_DELETE	0x1c /* delete an item from the playlist */
#define CMD_SEND_PLIST_EVENTS 0x1d /* request for playlist events */
#define CMD_PREV	0x20 /* start playing previous song if available */
#define CMD_SEND_PLIST	0x21 /* send the playlist to the requesting client */
#define CMD_GET_PLIST	0x22 /* get the playlist from one of the clients */
#define CMD_CAN_SEND_PLIST	0x23 /* mark the client as able to send
					playlist */
#define CMD_CLI_PLIST_ADD	0x24 /* add an item to the client's playlist */
#define CMD_CLI_PLIST_DEL	0x25 /* delete an item from the client's
					playlist */
#define CMD_CLI_PLIST_CLEAR	0x26 /* clear the client's playlist */
#define CMD_GET_SERIAL	0x27 /* get an unique serial number */
#define CMD_PLIST_SET_SERIAL	0x28 /* assign a serial number to the server's
					playlist */
#define CMD_LOCK	0x29 /* acquire a lock */
#define CMD_UNLOCK	0x2a /* release the lock */
#define CMD_PLIST_GET_SERIAL	0x2b /* get the serial number of the server's
					playlist */
#define CMD_GET_TAGS	0x2c /* get tags for the currently played file */
#define CMD_TOGGLE_MIXER_CHANNEL	0x2d /* toggle the mixer channel */
#define CMD_GET_MIXER_CHANNEL_NAME	0x2e /* get the mixer channel's name */
#define CMD_GET_FILE_TAGS	0x2f	/* get tags for the specified file */
#define CMD_ABORT_TAGS_REQUESTS	0x30	/* abort previous CMD_GET_FILE_TAGS
					   requests up to some file */
#define CMD_CLI_PLIST_MOVE	0x31	/* move an item */
#define CMD_LIST_MOVE		0x32	/* move an item */
#define CMD_GET_AVG_BITRATE	0x33	/* get the average bitrate */

#define CMD_TOGGLE_SOFTMIXER    0x34    /* toggle use of softmixer */
#define CMD_TOGGLE_EQUALIZER    0x35    /* toggle use of equalizer */
#define CMD_EQUALIZER_REFRESH   0x36    /* refresh EQ-presets */
#define CMD_EQUALIZER_PREV      0x37    /* select previous eq-preset */
#define CMD_EQUALIZER_NEXT      0x38    /* select next eq-preset */

#define CMD_TOGGLE_MAKE_MONO    0x39    /* toggle mono mixing */
#define CMD_JUMP_TO	0x3a /* jumps to a some position in the current stream */
#define CMD_SET_RATING	0x40 /* change rating for a file */

#define CMD_GET_OPTION_SHUFFLE	0x60
#define CMD_SET_OPTION_SHUFFLE	0x61
#define CMD_GET_OPTION_REPEAT	0x62
#define CMD_SET_OPTION_REPEAT	0x63
#define CMD_GET_OPTION_AUTONEXT	0x64
#define CMD_SET_OPTION_AUTONEXT	0x65

char *socket_name ();


class Socket
{
public:
	Socket(int sock, bool fatal_errors) : s(sock), f(fatal_errors), buffering(0)
	{ assert(sock >= 0); }

	int fd() const { return s; }

	void buffer() { ++buffering; }
	bool flush(); // send everything that was buffered
	bool is_buffering() const { return buffering; }

	void packet(int type)
	{
		logit ("packaging %X", type);
		assert(!buffering); buffer(); send(type); }
	void finish()
	{
		assert(buffering == 1);
		if (!buf.empty())
		{
			packets.emplace(std::move(buf));
			buf.clear();
		}
		buffering = 0;
	}
	int send_next_packet_noblock(); // 0:error, 1:packet sent, -1:would block
	std::queue<std::vector<char>> packets;

	template<typename T> bool send(T* x) { return send((const T*)x); }

	template<typename T> bool send(const T x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		logit("sending %d = 0x%X", (int)x, (int)x);
		return send(&x, sizeof(T)); }
	bool send(const str &s) { return send((size_t)s.length()) && send(s.data(), s.length()); }
	bool send(const char *s) { size_t n = s ? strlen(s) : 0; return send(n) && send(s, n); }
	bool send(const plist_item *i) { return send(i ? i->path : str()); }
	bool send(const plist &pl) { for (auto &i : pl.items) if (!send(CMD_CLI_PLIST_ADD) || !send(i.get())) return false; return true; }
	bool send(const file_tags *tags);

	template<typename T> bool get(T &x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		bool ok =read(&x, sizeof(T));
		logit("getting %d = 0x%X", (int)x, (int)x);
		return ok;
	}
	bool get(str &x)
	{
		size_t n; if (!get(n)) return false;
		x.resize(n); return read(&x[0], n);
	}
	int get_int() { int x; if (!get(x)) fatal ("Can't receive int value from socket!"); return x; }
	bool get_bool() { bool x; if (!get(x)) fatal ("Can't receive bool value from socket!"); return x; }
	str get_str() { str x; if (!get(x)) fatal ("Can't receive string from socket!"); return x; }
	file_tags *get_tags();

	bool get_int_noblock (int &i);

private:
	bool send(const void *data, size_t n);
	bool read(void *data, size_t n);

	int  s; // the actual socket handle
	bool f; // call fatal() on any send/recv error?
	int  buffering;
	std::vector<char> buf;
};
