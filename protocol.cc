#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include "protocol.h"
#include "playlist.h"
#include "files.h"

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
		result = fn (sock, buf, len, 0); \
		flags &= ~O_NONBLOCK; \
		if (fcntl (sock, F_SETFL, flags) == -1) \
			fatal ("Restoring flags for socket failed: %s", \
			        xstrerror (errno)); \
	} while (0)

/* Buffer used to send data in one bigger chunk instead of sending single
 * integer, string etc. values. */
struct packet_buf
{
	char *buf;
	size_t allocated;
	size_t len;
};

/* Create a socket name, return NULL if the name could not be created. */
char *socket_name ()
{
	char *socket_name = create_file_name (SOCKET_NAME);

	if (strlen(socket_name) > UNIX_PATH_MAX)
		fatal ("Can't create socket name!");

	return socket_name;
}

/* Get an integer value from the socket, return == 0 on error. */
int get_int (int sock, int *i)
{
	ssize_t res;

	res = recv (sock, i, sizeof(int), 0);
	if (res == -1)
		log_errno ("recv() failed when getting int", errno);

	return res == ssizeof(int) ? 1 : 0;
}

int get_bool (int sock, bool &f)
{
	ssize_t res;
	char c;
	res = recv (sock, &c, 1, 0);
	if (res == -1)
		log_errno ("recv() failed when getting bool", errno);
	f = c;
	return res == 1 ? 1 : 0;
}

/* Get an integer value from the socket without blocking. */
enum noblock_io_status get_int_noblock (int sock, int *i)
{
	ssize_t res;
	char *err;

	nonblocking (recv, res, sock, i, sizeof (int));

	if (res == ssizeof (int))
		return NB_IO_OK;
	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return NB_IO_BLOCK;

	err = xstrerror (errno);
	logit ("recv() failed when getting int (res %zd): %s", res, err);
	free (err);

	return NB_IO_ERR;
}

/* Send an integer value to the socket, return == 0 on error */
int send_int (int sock, int i)
{
	ssize_t res;

	res = send (sock, &i, sizeof(int), 0);
	if (res == -1)
		log_errno ("send() failed", errno);

	return res == ssizeof(int) ? 1 : 0;
}

/* Get the string from socket, return NULL on error. The memory is malloced. */
char *get_str (int sock)
{
	int len, nread = 0;
	char *str;

	if (!get_int(sock, &len))
		return NULL;

	if (!RANGE(0, len, MAX_SEND_STRING)) {
		logit ("Bad string length.");
		return NULL;
	}

	str = (char *)xmalloc (sizeof(char) * (len + 1));
	while (nread < len) {
		ssize_t res;

		res = recv (sock, str + nread, len - nread, 0);
		if (res == -1) {
			log_errno ("recv() failed when getting string", errno);
			free (str);
			return NULL;
		}
		if (res == 0) {
			logit ("Unexpected EOF when getting string");
			free (str);
			return NULL;
		}
		nread += res;
	}
	str[len] = 0;

	return str;
}

int send_str (int sock, const char *str)
{
	int len;

	len = strlen (str);
	if (!send_int (sock, len))
		return 0;

	if (send (sock, str, len, 0) != len)
		return 0;

	return 1;
}

/* Get a time_t value from the socket, return == 0 on error. */
int get_time (int sock, time_t *i)
{
	ssize_t res;

	res = recv (sock, i, sizeof(time_t), 0);
	if (res == -1)
		log_errno ("recv() failed when getting time_t", errno);

	return res == ssizeof(time_t) ? 1 : 0;
}

/* Send a time_t value to the socket, return == 0 on error */
int send_time (int sock, time_t i)
{
	ssize_t res;

	res = send (sock, &i, sizeof(time_t), 0);
	if (res == -1)
		log_errno ("send() failed", errno);

	return res == ssizeof(time_t) ? 1 : 0;
}

static struct packet_buf *packet_buf_new ()
{
	struct packet_buf *b;

	b = (struct packet_buf *)xmalloc (sizeof(struct packet_buf));
	b->buf = (char *)xmalloc (1024);
	b->allocated = 1024;
	b->len = 0;

	return b;
}

static void packet_buf_free (struct packet_buf *b)
{
	assert (b != NULL);

	free (b->buf);
	free (b);
}

/* Make sure that there is at least len bytes free. */
static void packet_buf_add_space (struct packet_buf *b, const size_t len)
{
	assert (b != NULL);

	if (b->allocated < b->len + len) {
		b->allocated += len + 256; /* put some more space */
		b->buf = (char *)xrealloc (b->buf, b->allocated);
	}
}

/* Add an integer value to the buffer */
static void packet_buf_add_int (struct packet_buf *b, const int n)
{
	assert (b != NULL);

	packet_buf_add_space (b, sizeof(n));
	memcpy (b->buf + b->len, &n, sizeof(n));
	b->len += sizeof(n);
}

/* Add an integer value to the buffer */
static void packet_buf_add_bool (struct packet_buf *b, bool f)
{
	packet_buf_add_space (b, 1);
	b->buf[b->len++] = f;
}

/* Add a string value to the buffer. */
static void packet_buf_add_str (struct packet_buf *b, const char *str)
{
	int str_len;

	assert (b != NULL);
	assert (str != NULL);

	str_len = strlen (str);

	packet_buf_add_int (b, str_len);
	packet_buf_add_space (b, str_len * sizeof(char));
	memcpy (b->buf + b->len, str, str_len * sizeof(char));
	b->len += str_len * sizeof(char);
}

/* Add a time_t value to the buffer. */
static void packet_buf_add_time (struct packet_buf *b, const time_t n)
{
	assert (b != NULL);

	packet_buf_add_space (b, sizeof(n));
	memcpy (b->buf + b->len, &n, sizeof(n));
	b->len += sizeof(n);
}

/* Add tags to the buffer. If tags == NULL, add empty tags. */
static void packet_buf_add_tags (struct packet_buf *b, const file_tags *tags)
{
	assert (b != NULL);

	if (tags) {
		packet_buf_add_bool(b, true);
		packet_buf_add_str (b, tags->title.c_str());
		packet_buf_add_str (b, tags->artist.c_str());
		packet_buf_add_str (b, tags->album.c_str());
		packet_buf_add_int (b, tags->track);
		packet_buf_add_int (b, tags->time);
		packet_buf_add_int (b, tags->rating);
	}
	else {
		packet_buf_add_bool(b, false);
	}
}

/* Add an item to the buffer. */
void packet_buf_add_item (struct packet_buf *b, const plist_item *item)
{
	packet_buf_add_str (b, item->path.c_str());
	packet_buf_add_tags (b, item->tags.get());
}

/* Send data to the socket. Return 0 on error. */
static int send_all (int sock, const char *buf, const size_t size)
{
	ssize_t sent;
	size_t send_pos = 0;

	while (send_pos < size) {
		sent = send (sock, buf + send_pos, size - send_pos, 0);
		if (sent < 0) {
			log_errno ("Error while sending data", errno);
			return 0;
		}
		send_pos += sent;
	}

	return 1;
}

/* Send a playlist item to the socket. If item == NULL, send empty item mark
 * (end of playlist). Return 0 on error. */
int send_item (int sock, const struct plist_item *item)
{
	int res = 1;
	struct packet_buf *b;

	if (!item) {
		if (!send_str(sock, "")) {
			logit ("Error while sending empty item");
			return 0;
		}
		return 1;
	}

	b = packet_buf_new ();
	packet_buf_add_item (b, item);
	if (!send_all(sock, b->buf, b->len)) {
		logit ("Error when sending item");
		res = 0;
	}

	packet_buf_free (b);
	return res;
}

struct file_tags *recv_tags (int sock)
{
	bool b; if (!get_bool(sock, b)){
		logit ("Error while receiving tags flag");
		return NULL;
	}
	if (!b) return NULL;

	file_tags *tags = new file_tags;

	char *s;
	#define STR(x) if (!(s = get_str(sock))) { logit ("Error while receiving tags"); delete tags; return NULL; } \
		tags->x = s; free(s)
	STR(title);
	STR(artist);
	STR(album);
	#undef STR

	if (!get_int(sock, &tags->track)) {
		logit ("Error while receiving track");
		delete tags;
		return NULL;
	}

	if (!get_int(sock, &tags->time)) {
		logit ("Error while receiving time");
		delete tags;
		return NULL;
	}

	if (!get_int(sock, &tags->rating)) {
		logit ("Error while receiving ratings");
		delete tags;
		return NULL;
	}

	return tags;
}

/* Send tags. If tags == NULL, send empty tags. Return 0 on error. */
int send_tags (int sock, const struct file_tags *tags)
{
	int res = 1;
	struct packet_buf *b;

	b = packet_buf_new ();
	packet_buf_add_tags (b, tags);

	if (!send_all(sock, b->buf, b->len))
		res = 0;

	packet_buf_free (b);
	return res;
}

/* Get a playlist item from the server.
 * The end of the playlist is indicated by item->file being an empty string.
 * The memory is malloc()ed.  Returns NULL on error. */
struct plist_item *recv_item (int sock)
{
	/* get the file name */
	char *path = get_str(sock);
	if (!path) {
		logit ("Error while receiving file name");
		return NULL;
	}
	if (!*path) { free(path); return NULL; }

	plist_item *item = new plist_item (path); free(path);
	item->tags.reset(recv_tags(sock));

	return item;
}

struct move_ev_data *recv_move_ev_data (int sock)
{
	struct move_ev_data *d;

	d = (struct move_ev_data *)xmalloc (sizeof(struct move_ev_data));

	if (!(d->from = get_str(sock))) {
		logit ("Error while receiving 'from' data");
		free (d);
		return NULL;
	}

	if (!(d->to = get_str(sock))) {
		logit ("Error while receiving 'to' data");
		free (d->from);
		free (d);
		return NULL;
	}

	return d;
}

/* Push an event on the queue if it's not already there. */
void event_push (struct event_queue *q, const int event, void *data)
{
	assert (q != NULL);

	if (!q->head) {
		q->head = (struct event *)xmalloc (sizeof(struct event));
		q->head->next = NULL;
		q->head->type = event;
		q->head->data = data;
		q->tail = q->head;
	}
	else {
		assert (q->head != NULL);
		assert (q->tail != NULL);
		assert (q->tail->next == NULL);

		q->tail->next = (struct event *)xmalloc (
				sizeof(struct event));
		q->tail = q->tail->next;
		q->tail->next = NULL;
		q->tail->type = event;
		q->tail->data = data;
	}
}

/* Remove the first event from the queue (don't free the data field). */
void event_pop (struct event_queue *q)
{
	struct event *e;

	assert (q != NULL);
	assert (q->head != NULL);
	assert (q->tail != NULL);

	e = q->head;
	q->head = e->next;
	free (e);

	if (q->tail == e)
		q->tail = NULL; /* the queue is empty */
}

/* Get the pointer to the first item in the queue or NULL if the queue is
 * empty. */
struct event *event_get_first (struct event_queue *q)
{
	assert (q != NULL);

	return q->head;
}

void free_tag_ev_data (struct tag_ev_response *d)
{
	assert (d != NULL);

	free (d->file);
	delete d->tags;
	free (d);
}
void free_rating_ev_data (struct rating_ev_response *d)
{
	assert (d != NULL);

	free (d->file);
	free (d);
}

void free_move_ev_data (struct move_ev_data *m)
{
	assert (m != NULL);
	assert (m->from != NULL);
	assert (m->to != NULL);

	free (m->to);
	free (m->from);
	free (m);
}

struct move_ev_data *move_ev_data_dup (const struct move_ev_data *m)
{
	struct move_ev_data *new_data;

	assert (m != NULL);
	assert (m->from != NULL);
	assert (m->to != NULL);

	new_data = (struct move_ev_data *)xmalloc (sizeof(struct move_ev_data));
	new_data->from = xstrdup (m->from);
	new_data->to = xstrdup (m->to);

	return new_data;
}

struct tag_ev_response *tag_ev_data_dup (const struct tag_ev_response *d)
{
	struct tag_ev_response *new_resp;

	assert (d != NULL);
	assert (d->file != NULL);

	new_resp = (struct tag_ev_response *)xmalloc (sizeof(struct tag_ev_response));
	new_resp->file = xstrdup (d->file);
	new_resp->tags = (d->tags ? new file_tags(*d->tags) : NULL);

	return new_resp;
}
struct rating_ev_response *rating_ev_data_dup (const struct rating_ev_response *d)
{
	struct rating_ev_response *new_resp;

	assert (d != NULL);
	assert (d->file != NULL);

	new_resp = (struct rating_ev_response *)xmalloc (sizeof(struct rating_ev_response));
	new_resp->file = xstrdup (d->file);
	new_resp->rating = d->rating;

	return new_resp;
}

/* Free data associated with the event if any. */
void free_event_data (const int type, void *data)
{
	if (type == EV_PLIST_ADD) {
		delete (plist_item *)data;
	}
	else if (type == EV_FILE_TAGS)
		free_tag_ev_data ((struct tag_ev_response *)data);
	else if (type == EV_PLIST_DEL || type == EV_STATUS_MSG
			|| type == EV_SRV_ERROR)
		free (data);
	else if (type == EV_PLIST_MOVE)
		free_move_ev_data ((struct move_ev_data *)data);
	else if (data)
		abort (); /* BUG */
}

/* Free event queue content without the queue structure. */
void event_queue_free (struct event_queue *q)
{
	struct event *e;

	assert (q != NULL);

	while ((e = event_get_first(q))) {
		free_event_data (e->type, e->data);
		event_pop (q);
	}
}

void event_queue_init (struct event_queue *q)
{
	assert (q != NULL);

	q->head = NULL;
	q->tail = NULL;
}

/* Return != 0 if the queue is empty. */
int event_queue_empty (const struct event_queue *q)
{
	assert (q != NULL);

	return q->head == NULL ? 1 : 0;
}

/* Make a packet buffer filled with the event (with data). */
static struct packet_buf *make_event_packet (const struct event *e)
{
	struct packet_buf *b;

	assert (e != NULL);

	b = packet_buf_new ();

	packet_buf_add_int (b, e->type);

	if (e->type == EV_PLIST_DEL
			|| e->type == EV_SRV_ERROR
			|| e->type == EV_STATUS_MSG) {
		assert (e->data != NULL);
		packet_buf_add_str (b, (const char*) e->data);
	}
	else if (e->type == EV_PLIST_ADD) {
		assert (e->data != NULL);
		packet_buf_add_item (b, (const plist_item*) e->data);
	}
	else if (e->type == EV_FILE_TAGS) {
		struct tag_ev_response *r;

		assert (e->data != NULL);
		r = (struct tag_ev_response*) e->data;

		packet_buf_add_str (b, r->file);
		packet_buf_add_tags (b, r->tags);
	}
	else if (e->type == EV_PLIST_MOVE) {
		struct move_ev_data *m;

		assert (e->data != NULL);

		m = (struct move_ev_data *)e->data;
		packet_buf_add_str (b, m->from);
		packet_buf_add_str (b, m->to);
	}
	else if (e->data)
		abort (); /* BUG */

	return b;
}

/* Send the first event from the queue and remove it on success.  If the
 * operation would block return NB_IO_BLOCK.  Return NB_IO_ERR on error
 * or NB_IO_OK on success. */
enum noblock_io_status event_send_noblock (int sock, struct event_queue *q)
{
	ssize_t res;
	char *err;
	struct packet_buf *b;
	enum noblock_io_status result;

	assert (q != NULL);
	assert (!event_queue_empty(q));

	b = make_event_packet (event_get_first(q));

	/* We must do it in one send() call to be able to handle blocking. */
	nonblocking (send, res, sock, b->buf, b->len);

	if (res == (ssize_t)b->len) {
		struct event *e;

		e = event_get_first (q);
		free_event_data (e->type, e->data);
		event_pop (q);

		result = NB_IO_OK;
		goto exit;
	}

	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		logit ("Sending event would block");
		result = NB_IO_BLOCK;
		goto exit;
	}

	err = xstrerror (errno);
	logit ("send()ing event failed (%zd): %s", res, err);
	free (err);
	result = NB_IO_ERR;

exit:
	packet_buf_free (b);
	return result;
}
