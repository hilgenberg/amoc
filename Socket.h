#pragma once
#include "playlist.h"
#include "server/protocol.h"

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
	Socket(int sock) : s(sock), buffering(0) { assert(sock >= 0); }

	int fd() const { return s; }

	void buffer() { ++buffering; }
	void flush(); // send everything that was buffered
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
		SOCKET_DEBUG(">>> buffering done (#%d)", (int)packets.size());
	}
	size_t pending() const { return packets.size(); }
	bool send_next_packet_noblock(); // true:packet sent, false:would block or nothing to send

	template<typename T> void send(T* x) { send((const T*)x); }

	template<typename T> void send(const T x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		SOCKET_DEBUG(">>> sending %d = 0x%X %s", (int)x, (int)x, buffering ? " (B)" : "");
		send(&x, sizeof(T)); }
	void send(const str &s) {
		SOCKET_DEBUG(">>> sending \"%s\" %s", s.c_str(), buffering ? " (B)" : "");
		send((size_t)s.length()); send(s.data(), s.length()); }
	void send(const char *s) {
		SOCKET_DEBUG(">>> sending \"%s\" %s", s ? s : "NULL", buffering ? " (B)" : "");
		size_t n = s ? strlen(s) : 0; send(n); send(s, n); }
	void send(const plist_item *i) { send(i ? i->path : str()); }
	void send(const plist &pl) { for (auto &i : pl.items) send(i->path); send(""); }
	void send(const file_tags *tags);
	void send(const tag_changes *tags);
	void send(ServerCommands c) { send((int)c); }
	void send(ServerEvents   c) { send((int)c); }
	
	void send(const std::set<int>    &idx); std::set<int>     get_idx_set();
	void send(const std::map<int,str> &ch); std::map<int,str> get_int_map();
	void send(const std::set<str>    &idx); std::set<str>     get_str_set();
	void send(const std::map<str,str> &ch); std::map<str,str> get_str_map();

	template<typename T> void get(T &x) {
		static_assert(std::is_integral<T>::value, "Integral required.");
		read(&x, sizeof(T));
		SOCKET_DEBUG("<<< getting %d = 0x%X", (int)x, (int)x);
	}
	void get(str &x)
	{
		size_t n; get(n);
		x.resize(n); read(&x[0], n);
		SOCKET_DEBUG("<<< getting \"%s\"", x.c_str());
	}
	void get(plist &plist)
	{
		stringlist S;
		while (true) {
			str s; get(s);
			if (s.empty()) break;
			S.push_back(std::move(s));
		}
		plist.clear();
		for (auto &s : S) plist += std::move(s);
	}
	int  get_int()  { int  x; get(x); return x; }
	bool get_bool() { bool x; get(x); return x; }
	str  get_str()  { str  x; get(x); return x; }
	file_tags *get_tags();
	tag_changes *get_tag_changes();

	bool get_int_noblock (int &i);

private:
	void send(const void *data, size_t n);
	void read(void *data, size_t n);

	int  s; // the actual socket handle

	int  buffering;
	std::vector<char> buf;
	
	std::queue<std::vector<char>> packets;

	struct BufferGuard
	{
		Socket &s; size_t n0; bool ok;
		BufferGuard(Socket &s) : s(s), n0(s.buf.size()), ok(false)
		{
			s.buffer();
		}
		void done() { ok = true; }
		~BufferGuard()
		{
			if (ok) s.flush(); else { s.buf.resize(n0); --s.buffering; }
		}
	};
	friend struct BufferGuard;
};
