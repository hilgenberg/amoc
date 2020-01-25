/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "ratings.h"
#include "server.h" /* for server_error in write method */

/* Ratings files should contain lines in this format:
 * [0-5] <filename>\n
 * Everything else is ignored.
 *
 * There must only be a single space after the rating, so that
 * files starting with spaces can be tagged without some
 * quoting scheme (we want parsing the file to be as fast as
 * possible).
 *
 * Newlines in file names are not handled in all cases (things
 * like "<something>\n3 <some other filename>", but whatever). */

/* We read files in chunks of BUF_SIZE bytes */
static constexpr size_t BUF_SIZE = (8*1024);


/* find rating for a file and returns that rating or
 * -1 if not found. If found, filepos is the position
 * of the rating character in rf.
 * rf is assumed to be freshly opened (i.e. ftell()==0). */
static int find_rating (const str &fn, FILE *rf, long *filepos)
{
	assert(!fn.empty() && rf && ftell(rf) == 0);

	char buf[BUF_SIZE]; /* storage for one chunk */
	char   *s = NULL;   /* current position in chunk */
	int     n = 0;      /* characters left in chunk */
	long fpos = 0;      /* ftell() of end of chunk */
	const int fnlen = (int)fn.length();

	/* get next char, refill buffer if needed */
	#define GETC(c) do{ \
	if (!n) { \
		n = fread(buf, 1, BUF_SIZE, rf); \
		s = buf; \
		fpos += n; \
		if (!n) (c) = -1; else { (c) = *(const unsigned char*)s++; --n; } \
	} \
	else { (c) = *(const unsigned char*)s++; --n; } \
	} while (0)
	
	/* loop over all lines in the file */
	while (true)
	{
		int c0; GETC(c0);

		if (c0 < 0) return -1; /* EOF */

		if (c0 == '\n') continue; /* empty line */

		if (c0 >= '0' && c0 <= '5') /* possible rating line */
		{
			char c; GETC(c);

			if (c < 0) return -1; /* EOF again */
			
			/* go straight to next line if we already read the newline */
			if (c == '\n') continue; 
			
			if (c == ' ') /* still good */
			{
				/* find fn */
				const char *t = fn.c_str(); /* remaining string to match */
				int nleft = fnlen; /* invariant: nleft == strlen(t) */
				while (true)
				{
					/* compare as much as possible in this chunk */
					int ncmp = (nleft < n ? nleft : n);
					if (memcmp(t, s, ncmp))
					{
						/* not our file. skip rest of line */
						break;
					}

					/* Note: next line is where things get weird
					 * if fn contains newlines */
					s += ncmp;
					t += ncmp;
					n -= ncmp;
					nleft -= ncmp;

					if (!nleft)
					{
						/* remember position of rating */
						if (filepos)
							*filepos = fpos-n - fnlen - 2;
						
						/* check for trailing garbage */
						GETC(c);
						if (c >= 0 && c != '\n')
							break; /* skip rest of line */

						/* success */
						return c0 - '0';
					}

					if (!n) /* read next chunk */
					{
						n = fread(buf, 1, BUF_SIZE, rf);
						if (!n) return -1;
						s = buf;
					}
				}
			}
		}

		/* skip to next line */
		while (true)
		{
			char *e = (char*) memchr (s, '\n', n);
			if (e)
			{
				/* found a newline, update position in buffer */
				n -= e-s + 1;
				s = e+1;
				break;
			}
			/* look for newline in next chunk */
			n = fread(buf, 1, BUF_SIZE, rf);
			if (!n) return -1;
			s = buf;
		}
	}
	#undef GETC
}

static str ratings_file(const str &fn)
{
	assert(!fn.empty());
	return add_path(containing_directory(fn), options::RatingFile);
}

/* read rating for a file */
int ratings_read (const str &fn)
{
	assert(!fn.empty());

	str  rfp = ratings_file(fn);
	FILE *rf = fopen (rfp.c_str(), "rb");
	if (!rf) return 0;

	int rating = find_rating (file_name(fn), rf, NULL);
	fclose (rf);

	/* if fn has no rating, treat as 0-rating */
	return rating < 0 ? 0 : rating;
}

int ratings_remove (const str &fn)
{
	assert(!fn.empty());

	str  rfp = ratings_file(fn);
	FILE *rf = fopen (rfp.c_str(), "rb+");
	if (!rf) return -1;

	str fnn = file_name(fn);
	long filepos; int r = find_rating (fnn, rf, &filepos);
	if (r < 0) { fclose(rf); return -1; }

	size_t len = 3+file_name(fnn).length(); // rating, space, filename, newline
	fseek(rf, 0L, SEEK_END); size_t filesize = ftell(rf);
	if (filesize <= len)
	{
		fclose(rf);
		unlink(rfp.c_str());
		return r;
	}

	if (filepos + len < filesize)
	{
		char buf[BUF_SIZE]; /* storage for one chunk */
		size_t i = filepos+len, o = filepos, n = filesize-filepos-len;
		while (n)
		{
			fseek(rf, i, SEEK_SET);
			size_t k = std::min(BUF_SIZE, n);
			k = fread(buf, 1, k, rf);
			if (!k)
			{
				// bad... file is half-done and broken now
				logit("ratings update failed (cannot read)");
				fclose(rf);
				return r;
			}

			fseek(rf, o, SEEK_SET);
			size_t kk = fwrite(buf, 1, k, rf);
			if (kk != k)
			{
				// bad... file is half-done and broken now
				logit("ratings update failed (cannot write)");
				fclose(rf);
				return r;
			}
			o += k;
			i += k;
			n -= k;
		}
	}
	fclose(rf);
	truncate(rfp.c_str(), std::max((size_t)filepos, filesize-len)); // handles missing newline

	return r;
}

/* update ratings file for given file path and rating */
bool ratings_write (const str &path, int rating)
{
	assert(!path.empty() && rating >= 0 && rating <= 5);

	const char *failmsg = "Rating could not be written (check permissions).";
	#define FAIL do { \
		server_error (__FILE__, __LINE__, "ratings_write_file", failmsg); \
		return false; } while (0)

	str fn = file_name(path);

	str  rfp = ratings_file(path);
	FILE *rf = fopen (rfp.c_str(), "rb+");
	if (!rf)
	{
		if (rating <= 0) return true; /* 0 rating needs no writing */

		/* ratings file did not exist or could not be opened
		 * for reading. Try creating it */
		rf = fopen (rfp.c_str(), "ab");
		if (!rf) FAIL; /* can't create it either */

		/* append new rating */
		int ok = fprintf (rf, "%d %s\n", rating, fn.c_str());
		fclose (rf);
		if (!ok) FAIL;
		return true;
	}

	/* ratings file exists, locate our file */
	long filepos;
	int  ok = 1;
	int r0 = find_rating (fn, rf, &filepos);
	if (r0 < 0)
	{
		/* not found - append */
		if (rating > 0 && 0 == fseek (rf, 0, SEEK_END))
		{
			ok = fprintf (rf, "%d %s\n", rating, fn.c_str());
		}
	}
	else if (r0 != rating)
	{
		/* update existing entry */
		assert (rating >= 0 && rating <= 5);
		if (0 == fseek (rf, filepos, SEEK_SET))
		{
			ok = (fputc ('0' + rating, rf) > 0);
		}
	}
	fclose (rf);

	if (!ok) FAIL;

	#undef FAIL

	return true;
}

bool ratings_move(const str &src, const str &dst)
{
	if (src == dst) return true;
	int r = ratings_remove(src);
	return r <= 0 || ratings_write(dst, r);
}
