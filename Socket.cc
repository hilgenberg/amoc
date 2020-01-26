#include "Socket.h"
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>

#define nonblocking(fn, result, sock, buf, len) \
	do { \
		long flags = fcntl (sock, F_GETFL); \
		if (flags == -1) \
			fatal ("Getting flags for socket failed: %s", \
			        xstrerror (errno)); \
		flags |= O_NONBLOCK; \
		if (fcntl (sock, F_SETFL, O_NONBLOCK) == -1) \
			fatal ("Setting O_NONBLOCK for the socket failed: %s", \
			        xstrerror (errno)); \
		result = ::fn (sock, buf, len, 0); \
		flags &= ~O_NONBLOCK; \
		if (fcntl (sock, F_SETFL, flags) == -1) \
			fatal ("Restoring flags for socket failed: %s", \
			        xstrerror (errno)); \
	} while (0)

void Socket::send(const void *data, size_t n)
{
	SOCKET_DEBUG("Socketsend: %d %s", (int)n, buffering ? "-> buffer" : "");
	if (buffering)
	{
		buf.insert(buf.end(), (char*)data, (char*)data+n);
	}
	else while (n)
	{
		ssize_t res = ::send(s, data, n, 0);
		if (res <= 0) 
		{
			log_errno ("Socket send() failed", errno);
			throw std::runtime_error("Socket send() failed!");
		}
		n -= res;
		(char*&)data += res;
	}
}

void Socket::read(void *data, size_t n)
{
	SOCKET_DEBUG("Socketread: %d", n);
	while (n)
	{
		ssize_t res = recv (s, data, n, 0);
		SOCKET_DEBUG("Socketread: %d -- %d", n, (int)res);
		if (res < 0)
		{
			log_errno ("Socket recv() failed", errno);
			throw std::runtime_error("Socket recv() failed!");
		}
		if (res == 0)
		{
			log_errno ("Unexpected EOF from socket recv()!", errno);
			throw std::runtime_error("Unexpected EOF from socket recv()!");
		}
		n -= res;
		(char*&)data += res;
	}
}

void Socket::flush()
{
	if (!buffering) { assert(false); return; }
	if (--buffering || buf.empty()) return;
	try {
		send (buf.data(), buf.size());
	}
	catch (...)
	{
		buf.clear(); // even on errors
		throw;
	}
	buf.clear();
}

bool Socket::get_int_noblock (int &i)
{
	ssize_t res;
	nonblocking (recv, res, s, &i, sizeof (int));

	if (res == ssizeof (int)) return true;
	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;

	char *err = xstrerror (errno);
	logit ("recv() failed when getting int (res %zd): %s", res, err);
	free (err);
	throw std::runtime_error("Nonblocking Socket recv() failed!");
}

void Socket::send(const file_tags *tags)
{
	if (!tags) { send(false); return; }
	buffer(); size_t n0 = buf.size();
	try {
		send(true); send(tags->title); send(tags->artist);
		send(tags->album); send(tags->track);
		send(tags->time); send(tags->rating);
	}
	catch (...)
	{
		buf.resize(n0); --buffering;
		throw;
	}
	flush();
}

file_tags *Socket::get_tags()
{
	bool b; get(b); if (!b) return NULL;

	file_tags *tags = new file_tags;
	try {
		get(tags->title); get(tags->artist);
		get(tags->album); get(tags->track);
		get(tags->time);  get(tags->rating);
	}
	catch (...)
	{
		delete tags;
		throw;
	}
	return tags;
}

/* Send the first event from the queue and remove it on success.  If the
 * operation would block return NB_IO_BLOCK.  Return NB_IO_ERR on error
 * or NB_IO_OK on success. */
bool Socket::send_next_packet_noblock()
{
	if (packets.empty()) return false;
	const std::vector<char> &d = packets.front();

	SOCKET_DEBUG("sending packet %X (%d)", *(int*)d.data(), (int)d.size());

	/* We must do it in one send() call to be able to handle blocking. */
	ssize_t res;
	nonblocking(send, res, s, d.data(), d.size());

	if (res == (ssize_t)d.size())
	{
		packets.pop();
		return true;
	}

	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
		logit ("Sending event would block");
		return false;
	}

	char *err = xstrerror (errno);
	logit ("send()ing event failed (%zd): %s", res, err);
	free (err);
	throw std::runtime_error("send_next_packet_noblock failed");
}
