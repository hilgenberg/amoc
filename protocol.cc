#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>

#include "protocol.h"
#include "playlist.h"

/* Maximal socket name. */
#define UNIX_PATH_MAX	108
#define SOCKET_NAME	"socket2"

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


/* Create a socket name, return NULL if the name could not be created. */
char *socket_name ()
{
	char *socket_name = create_file_name (SOCKET_NAME);

	if (strlen(socket_name) > UNIX_PATH_MAX)
		fatal ("Can't create socket name!");

	return socket_name;
}


bool Socket::send(const void *data, size_t n)
{
	//logit("Socketsend: %d %s", (int)n, buffering ? "-> buffer" : "");
	if (buffering)
	{
		buf.insert(buf.end(), (char*)data, (char*)data+n);
	}
	else while (n)
	{
		ssize_t res = ::send(s, data, n, 0);
		if (res <= 0) 
		{
			if (f) fatal ("Socket send() failed!");
			log_errno ("Socket send() failed", errno);
			return false;
		}
		n -= res;
		(char*&)data += res;
	}
	return true;
}
bool Socket::read(void *data, size_t n)
{
	//logit("Socketread: %d", n);
	if (!n) return true;
	while (n)
	{
		ssize_t res = recv (s, data, n, 0);
		if (res == -1)
		{
			if (f) fatal ("Socket recv() failed!");
			log_errno ("Socket recv() failed", errno);
			return false;
		}
		if (n == 0)
		{
			if (f) fatal ("Unexpected EOF from socket recv()!");
			log_errno ("Unexpected EOF from socket recv()!", errno);
			return false;
		}
		n -= res;
		(char*&)data += res;
	}
	return true;
}

bool Socket::flush()
{
	if (!buffering) { assert(false); return true; }
	if (--buffering) return true;
	if (buf.empty()) return true;
	bool ok = send (buf.data(), buf.size());
	buf.clear(); // even on errors
	return ok;
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

	if (f) fatal ("Socket recv() failed!");
	return false;
}

bool Socket::send(const file_tags *tags)
{
	if (!tags) return send(false);
	buffer(); size_t n0 = buf.size();
	bool ok = send(true) && send(tags->title) && send(tags->artist) &&
		send(tags->album) && send(tags->track) &&
		send(tags->time) && send(tags->rating);
	if (!ok){ buf.resize(n0); --buffering; return false; }
	return flush();
}

file_tags *Socket::get_tags()
{
	bool b; if (!get(b)) return NULL;
	if (!b) return NULL;

	file_tags *tags = new file_tags;
	bool ok  = get(tags->title) && get(tags->artist) &&
		get(tags->album) && get(tags->track) &&
		get(tags->time) && get(tags->rating);
	if (!ok) { delete tags; return NULL; }
	return tags;
}

/* Send the first event from the queue and remove it on success.  If the
 * operation would block return NB_IO_BLOCK.  Return NB_IO_ERR on error
 * or NB_IO_OK on success. */
int Socket::send_next_packet_noblock()
{
	if (packets.empty()) return 1;
	const std::vector<char> &d = packets.front();


	logit("sending packet %X (%d)", *(int*)d.data(), (int)d.size());


	/* We must do it in one send() call to be able to handle blocking. */
	ssize_t res;
	nonblocking (send, res, s, d.data(), d.size());

	if (res == (ssize_t)d.size())
	{
		packets.pop();
		return 1;
	}

	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
		logit ("Sending event would block");
		return -1;
	}

	char *err = xstrerror (errno);
	logit ("send()ing event failed (%zd): %s", res, err);
	free (err);
	return 0;
}
