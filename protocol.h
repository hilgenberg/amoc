#pragma once

#include "playlist.h"

/* Definition of events sent by server to the client. */
enum ServerEvents : int
{
	EV_PONG = 1,	/* response for CMD_PING */
	EV_BUSY,	/* too many clients are connected to the server */
	EV_EXIT,	/* the server is about to exit */
	
	EV_SRV_ERROR,	/* an error occurred */
	EV_STATUS_MSG,	/* followed by a status message */
	
	EV_DATA,	/* data in response to a request follows */
	EV_FILE_TAGS,	/* tags in a response for tags request */
	EV_FILE_RATING,	/* ratings changed for a file */
	
	EV_PLIST_NEW,	/* replaced the playlist (no data. use CMD_PLIST_GET) */
	EV_PLIST_ADD,	/* items were added, followed by the file names and "" */
	EV_PLIST_DEL,	/* an item was deleted, followed by the index */
	EV_PLIST_MOVE,	/* an item moved, followed by its old and new indices */

	EV_AUDIO_START,	/* playing of audio has started */
	EV_AUDIO_STOP,	/* playing of audio has stopped */
	EV_STATE, 	/* server has changed the play/pause/stopped state */
	EV_CTIME,	/* current time of the song has changed */
	EV_BITRATE,	/* the bitrate has changed */
	EV_RATE,	/* the rate has changed */
	EV_CHANNELS,	/* the number of channels has changed */
	EV_OPTIONS,	/* the options (repeat, shuffle, autonext) have changed */
	EV_TAGS,	/* tags for the current file have changed */
	EV_AVG_BITRATE,	/* average bitrate has changed (new song) */
	EV_MIXER_CHANGE	/* the mixer channel was changed */
};

/* Definition of server commands. */
enum ServerCommands : int
{
	CMD_PING,		/* request for EV_PONG */
	CMD_QUIT,		/* shutdown the server */
	CMD_DISCONNECT,		/* disconnect from the server */

	CMD_PLAY,		/* play i'th item on the (optionally following) playlist, or if -1, play the following path */
	CMD_STOP,		/* stop playing */
	CMD_PAUSE,		/* pause */
	CMD_UNPAUSE,		/* unpause */
	CMD_NEXT,		/* start playing next song if available */
	CMD_PREV,		/* start playing previous song if available */
	CMD_SEEK,		/* seek in the current stream */
	CMD_JUMP_TO,		/* jumps to a some position in the current stream */

	CMD_PLIST_GET,		/* send the entire playlist to client */
	CMD_PLIST_ADD,		/* add following items to the playlist */
	CMD_PLIST_DEL,		/* delete an item from the server's playlist */
	CMD_PLIST_MOVE,		/* move an item */
	CMD_GET_FILE_TAGS,	/* get tags for the specified file */
	CMD_ABORT_TAGS_REQUESTS,/* abort all previous CMD_GET_FILE_TAGS requests */
	CMD_SET_RATING,		/* change rating for a file */

	CMD_GET_TAGS,		/* get tags for the current file (gets an immediate EV_DATA reply) */
	CMD_GET_CTIME,		/* get the current song time */
	CMD_GET_SNAME,		/* get the stream file name */
	CMD_GET_STATE,		/* get the state */
	CMD_GET_BITRATE,	/* get the bitrate */
	CMD_GET_RATE,		/* get the rate */
	CMD_GET_CHANNELS,	/* get the number of channels */
	CMD_GET_MIXER,		/* get the volume level */
	CMD_SET_MIXER,		/* set the volume level */
	CMD_GET_AVG_BITRATE,	/* get the average bitrate */
	CMD_GET_MIXER_CHANNEL_NAME,/* get the mixer channel's name */

	CMD_TOGGLE_MIXER_CHANNEL,/* toggle the mixer channel */
	CMD_TOGGLE_SOFTMIXER,	/* toggle use of softmixer */
	CMD_TOGGLE_EQUALIZER,	/* toggle use of equalizer */
	CMD_EQUALIZER_REFRESH,	/* refresh EQ-presets */
	CMD_EQUALIZER_PREV,	/* select previous eq-preset */
	CMD_EQUALIZER_NEXT,	/* select next eq-preset */
	CMD_TOGGLE_MAKE_MONO,	/* toggle mono mixing */

	CMD_GET_OPTION_SHUFFLE, CMD_GET_OPTION_REPEAT, CMD_GET_OPTION_AUTONEXT,
	CMD_SET_OPTION_SHUFFLE, CMD_SET_OPTION_REPEAT, CMD_SET_OPTION_AUTONEXT
};

char *socket_name ();

class Socket
{
public:
	Socket(int sock, bool fatal_errors) : s(sock), f(fatal_errors), buffering(0) { assert(sock >= 0); }

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
	bool send(const plist &pl) { for (auto &i : pl.items) if (!send(i->path)) return false; return send(""); }
	bool send(const file_tags *tags);
	bool send(ServerCommands c) { return send((int)c); }
	bool send(ServerEvents   c) { return send((int)c); }

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
	bool get(plist &plist)
	{
		stringlist S;
		while (true) {
			str s; if (!get(s)) return false;
			if (s.empty()) break;
			S.push_back(std::move(s));
		}
		plist.clear();
		for (auto &s : S) plist += std::move(s);
		return true;
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
