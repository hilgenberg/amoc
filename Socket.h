#pragma once
#include "playlist.h"
#include "protocol.h"

#if 0
#define SOCKET_DEBUG logit
#else
#define SOCKET_DEBUG(...) 
#endif

//---------------------------------------------------------------
// Socket sends data between the client and server. Optionally
// buffering many smaller pieces into one larger send() call.
// It can also do packaging, which creates buffered chunks with
// a header that get stored in a queue until they can be sent
// without blocking (used by the server for almost everything
// it sends to the clients).
//
// Main pieces are:
// - s: a socket handle
// - buf: the buffer, which can be moved into...
// - packets: the package queue
//
// Errors either call fatal (if the option is set in c'tor) or
// just return false. (TODO: should be exceptions instead).
//---------------------------------------------------------------

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
		SOCKET_DEBUG(">>> packaging %X", type);
		assert(!buffering); buffer(); send(type);
	}
	void finish()
	{
		assert(buffering == 1);
		if (!buf.empty())
		{
			packets.emplace(std::move(buf));
			buf.clear();
		}
		buffering = 0;
		SOCKET_DEBUG(">>> buffering done");
	}
	int send_next_packet_noblock(); // 0:error, 1:packet sent, -1:would block
	std::queue<std::vector<char>> packets;

	template<typename T> bool send(T* x) { return send((const T*)x); }

	template<typename T> bool send(const T x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		SOCKET_DEBUG(">>> sending %d = 0x%X %s", (int)x, (int)x, buffering ? " (B)" : "");
		return send(&x, sizeof(T)); }
	bool send(const str &s) {
		SOCKET_DEBUG(">>> sending \"%s\" %s", s.c_str(), buffering ? " (B)" : "");
		return send((size_t)s.length()) && send(s.data(), s.length()); }
	bool send(const char *s) {
		SOCKET_DEBUG(">>> sending \"%s\" %s", s ? s : "NULL", buffering ? " (B)" : "");
		size_t n = s ? strlen(s) : 0; return send(n) && send(s, n); }
	bool send(const plist_item *i) { return send(i ? i->path : str()); }
	bool send(const plist &pl) { for (auto &i : pl.items) if (!send(i->path)) return false; return send(""); }
	bool send(const file_tags *tags);
	bool send(ServerCommands c) { return send((int)c); }
	bool send(ServerEvents   c) { return send((int)c); }

	template<typename T> bool get(T &x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		bool ok =read(&x, sizeof(T));
		SOCKET_DEBUG("<<< getting %d = 0x%X", (int)x, (int)x);
		return ok;
	}
	bool get(str &x)
	{
		size_t n; if (!get(n)) return false;
		x.resize(n); bool ok = read(&x[0], n);
		SOCKET_DEBUG("<<< getting \"%s\"", x.c_str());
		return ok;
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
