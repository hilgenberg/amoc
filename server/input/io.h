#pragma once

#include <sys/types.h>
#include <pthread.h>

struct io_stream
{
	io_stream(const char *file); // throws runtime_error on error
	~io_stream();

	ssize_t read(void *buf, size_t count);
	ssize_t peek(void *buf, size_t count);
	off_t   seek(off_t offset, int whence);
	off_t   tell() const { return pos; }

	int     fd;
	off_t   pos;        /* current position in the file from the user point of view */
	off_t   size;       /* size of the file */
	bool    eof;        /* was the end of file reached? */
};

inline ssize_t io_read(io_stream *s, void *buf, size_t count) { assert(s); return s->read(buf, count); }
inline ssize_t io_peek(io_stream *s, void *buf, size_t count) { assert(s); return s->peek(buf, count); }
inline off_t   io_seek(io_stream *s, off_t offset, int whence) { assert(s); return s->seek(offset, whence); }
inline off_t   io_file_size(const io_stream *s) { assert(s); return s->size; }
inline off_t   io_tell(io_stream *s) { assert(s); return s->tell(); }
inline int     io_eof(io_stream *s) { assert(s); return s->eof; }
