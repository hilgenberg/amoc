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
#include <inttypes.h>
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif
#include "io.h"

io_stream::io_stream(const char *file)
: fd(-1)
, pos(0)
, size(0)
, eof(false)
{
	fd = open (file, O_RDONLY);
	if (fd >= 0)
	{
		struct stat file_stat;
		if (fstat(fd, &file_stat) != -1)
		{
			size = file_stat.st_size;
			return;
		}
		close(fd);
	}
	char *s = xstrerror(errno);
	std::string ret(s);
	free(s);
	throw std::runtime_error(ret);
}

io_stream::~io_stream()
{
	if (fd >= 0) close(fd);
}

off_t io_stream::seek(off_t offset, int whence)
{
	off_t new_pos = 0;
	switch (whence) {
	case SEEK_SET: new_pos = offset; break;
	case SEEK_CUR: new_pos = pos + offset; break;
	case SEEK_END: new_pos = size + offset; break;
	default: fatal ("Bad whence value: %d", whence);
	}
	new_pos = CLAMP(0, new_pos, size);
	off_t res = lseek(fd, new_pos, SEEK_SET);
	if (res != -1)
	{
		pos = res;
		eof = (pos >= size);
		debug ("Seek to: %" PRId64, res);
	}
	else
		logit ("Seek error");
	return res;
}

ssize_t io_stream::read(void *buf, size_t count)
{
	assert(buf);
	ssize_t res = ::read(fd, buf, count);
	if (res < 0) return -1;
	if (res == 0) eof = 1;
	pos += res;
	return res;
}

ssize_t io_stream::peek(void *buf, size_t count)
{
	assert(buf);
	ssize_t res = ::read(fd, buf, count);
	if (res < 0) return -1;
	if (lseek(fd, -res, SEEK_CUR) < 0) return -1;
	return res;
}
