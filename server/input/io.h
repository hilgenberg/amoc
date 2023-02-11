#pragma once

#include <sys/types.h>
#include <pthread.h>
#include "../../fifo_buf.h"

enum io_source
{
	IO_SOURCE_FD,
	IO_SOURCE_MMAP
};

struct io_stream
{
	enum io_source source;	/* source of the file */
	int fd;
	off_t size;	/* size of the file */
	int errno_val;	/* errno value of the last operation  - 0 if ok */
	int read_error; /* set to != 0 if the last read operation dailed */
	char *strerror;	/* error string */
	int opened;	/* was the stream opened (open(), mmap(), etc.)? */
	int eof;	/* was the end of file reached? */
	off_t pos;	/* current position in the file from the user point of view */
	pthread_mutex_t io_mtx;	/* mutex for IO operations */

#ifdef HAVE_MMAP
	void *mem;
	off_t mem_pos;
#endif
};

struct io_stream *io_open (const char *file);
ssize_t io_read (struct io_stream *s, void *buf, size_t count);
ssize_t io_peek (struct io_stream *s, void *buf, size_t count);
off_t io_seek (struct io_stream *s, off_t offset, int whence);
void io_close (struct io_stream *s);
int io_ok (struct io_stream *s);
char *io_strerror (struct io_stream *s);
off_t io_file_size (const struct io_stream *s);
off_t io_tell (struct io_stream *s);
int io_eof (struct io_stream *s);
