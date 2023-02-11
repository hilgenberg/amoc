/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* TODO:
 * - handle SIGBUS (mmap() read error)
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <inttypes.h>

#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif

#include "io.h"

#ifdef HAVE_MMAP
static void *io_mmap_file (const struct io_stream *s)
{
	if (s->size < 1 || (uint64_t)s->size > SIZE_MAX) {
		logit ("File size unsuitable for mmap()");
		return NULL;
	}

	void *result = mmap (0, (size_t)s->size, PROT_READ, MAP_SHARED, s->fd, 0);
	if (result == MAP_FAILED) {
		log_errno ("mmap() failed", errno);
		return NULL;
	}

	return result;
}

static ssize_t io_read_mmap (struct io_stream *s, const int dont_move,
		void *buf, size_t count)
{
	struct stat file_stat;

	assert (s->mem != NULL);

	if (fstat (s->fd, &file_stat) == -1) {
		log_errno ("fstat() failed", errno);
		return -1;
	}

	if (s->size != file_stat.st_size) {
		logit ("File size has changed");

		if (munmap (s->mem, (size_t)s->size)) {
			log_errno ("munmap() failed", errno);
			return -1;
		}

		s->size = file_stat.st_size;
		s->mem = io_mmap_file (s);
		if (!s->mem)
			return -1;

		if (s->mem_pos > s->size)
			logit ("File shrunk");
	}

	if (s->mem_pos >= s->size)
	{
		if (!dont_move) s->eof = true;
		return 0;
	}

	size_t to_read = MIN(count, (size_t) (s->size - s->mem_pos));
	memcpy (buf, (char *)s->mem + s->mem_pos, to_read);

	if (!dont_move) s->mem_pos += to_read;

	return to_read;
}
#endif


off_t io_seek (struct io_stream *s, off_t offset, int whence)
{
	off_t res, new_pos = 0;

	assert (s != NULL);
	assert (s->opened);

	if (!io_ok(s))
		return -1;

	LOCK (s->io_mtx);
	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = s->pos + offset;
		break;
	case SEEK_END:
		new_pos = s->size + offset;
		break;
	default:
		fatal ("Bad whence value: %d", whence);
	}

	new_pos = CLAMP(0, new_pos, s->size);

	switch (s->source) {
#ifdef HAVE_MMAP
	case IO_SOURCE_MMAP:
		res = (s->mem_pos = new_pos);
		s->eof = (s->mem_pos >= s->size);
		break;
#endif
	case IO_SOURCE_FD:
		s->eof = false;
		res = lseek (s->fd, new_pos, SEEK_SET);
		break;
	default:
		fatal ("Unknown io_stream->source: %d", s->source);
	}

	if (res != -1)
		s->pos = res;
	UNLOCK (s->io_mtx);

	if (res != -1)
		debug ("Seek to: %" PRId64, res);
	else
		logit ("Seek error");

	return res;
}

/* Close the stream and free all resources associated with it. */
void io_close (struct io_stream *s)
{
	int rc;

	assert (s != NULL);

	logit ("Closing stream...");

	if (s->opened) {
		switch (s->source) {
		case IO_SOURCE_FD:
			close (s->fd);
			break;
#ifdef HAVE_MMAP
		case IO_SOURCE_MMAP:
			if (s->mem && munmap (s->mem, (size_t)s->size))
				log_errno ("munmap() failed", errno);
			close (s->fd);
			break;
#endif
		default:
			fatal ("Unknown io_stream->source: %d", s->source);
		}

		s->opened = 0;
	}

	rc = pthread_mutex_destroy (&s->io_mtx);
	if (rc != 0)
		log_errno ("Destroying io_mtx failed", rc);

	if (s->strerror)
		free (s->strerror);
	free (s);

	logit ("done");
}

static void io_open_file (struct io_stream *s, const char *file)
{
	struct stat file_stat;

	s->source = IO_SOURCE_FD;

	do {
		s->fd = open (file, O_RDONLY);
		if (s->fd == -1) {
			s->errno_val = errno;
			break;
		}

		if (fstat (s->fd, &file_stat) == -1) {
			s->errno_val = errno;
			close (s->fd);
			break;
		}

		s->size = file_stat.st_size;
		s->opened = 1;

#ifdef HAVE_MMAP
		if (!options::UseMMap) {
			logit ("Not using mmap()");
			s->mem = NULL;
			break;
		}

		s->mem = io_mmap_file (s);
		if (!s->mem)
			break;

		s->source = IO_SOURCE_MMAP;
		s->mem_pos = 0;
#endif
	} while (0);
}

/* Open the file. */
struct io_stream *io_open (const char *file)
{

	assert (file != NULL);

	struct io_stream *s = (struct io_stream*) xmalloc (sizeof(struct io_stream));
	s->errno_val = 0;
	s->read_error = 0;
	s->strerror = NULL;
	s->opened = 0;
	s->size = -1;

	io_open_file (s, file);

	pthread_mutex_init (&s->io_mtx, NULL);

	if (!s->opened)
		return s;

	s->eof = 0;
	s->pos = 0;

	return s;
}

/* Return non-zero if the stream was free of errors. */
int io_ok (struct io_stream *s)
{
	return !s->read_error && s->errno_val == 0;
}

/* Read data from the stream without buffering. If dont_move was set, the
 * stream position is unchanged. */
static ssize_t io_read_unbuffered (struct io_stream *s, const int dont_move,
		void *buf, size_t count)
{
	assert (!s->eof);
	assert (s != NULL);
	assert (buf != NULL);

	switch (s->source) {
	case IO_SOURCE_FD:
	{
		ssize_t res = read (s->fd, buf, count);
		if (res < 0) return -1;
		if (dont_move)
		{
			if (lseek(s->fd, -res, SEEK_CUR) < 0) return -1;
		}
		else
		{
			s->pos += res;
			if (res == 0) s->eof = 1;
		}
		return res;
	}
#ifdef HAVE_MMAP
	case IO_SOURCE_MMAP:
	{
		ssize_t res = io_read_mmap (s, dont_move, buf, count);
		if (!dont_move) s->pos += res;

		return res;
	}
#endif
	default:
		fatal ("Unknown io_stream->source: %d", s->source);
	}
}

/* Read data from the stream to the buffer of size count.  Return the number
 * of bytes read, 0 on EOF, < 0 on error. */
ssize_t io_read (struct io_stream *s, void *buf, size_t count)
{
	assert (s != NULL);
	assert (buf != NULL);
	assert (s->opened);

	if (s->eof) return 0;
	return io_read_unbuffered (s, 0, buf, count);
}

/* Read data from the stream to the buffer of size count. The data are not
 * removed from the stream. Return the number of bytes read, 0 on EOF, < 0
 * on error. */
ssize_t io_peek (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received;

	assert (s != NULL);
	assert (buf != NULL);

	received = io_read_unbuffered (s, 1, buf, count);

	return io_ok(s) ? received : -1;
}

/* Get the string describing the error associated with the stream. */
char *io_strerror (struct io_stream *s)
{
	if (s->strerror)
		free (s->strerror);

	if (s->errno_val)
		s->strerror = xstrerror (s->errno_val);
	else
		s->strerror = xstrdup ("OK");

	return s->strerror;
}

/* Get the file size if available or -1. */
off_t io_file_size (const struct io_stream *s)
{
	assert (s != NULL);

	return s->size;
}

/* Return the stream position. */
off_t io_tell (struct io_stream *s)
{
	off_t res = -1;

	assert (s != NULL);

	res = s->pos;

	return res;
}

/* Return != 0 if we are at the end of the stream. */
int io_eof (struct io_stream *s)
{
	assert (s != NULL);

	return s->eof;
}
