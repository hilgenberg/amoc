/*
 * MOC - music on console
 * Copyright (C) 2004 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <time.h>
#include <sys/types.h>
#include <pthread.h>
#include <signal.h>
#include <syslog.h>

#include "common.h"
#include "server/server.h"
#include "client/interface.h"

static int im_server = 0; /* Am I the server? */

void internal_error (const char *file, int line, const char *function,
                     const char *format, ...)
{
	int saved_errno = errno;
	va_list va;
	char *msg;

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);

	if (im_server)
		server_error (file, line, function, msg);
	else
		fputs(msg, stderr);
		//interface_error (msg); -- TODO

	free (msg);

	errno = saved_errno;
}

/* End program with a message. Use when an error occurs and we can't recover.
 * If we're the server, then also log the message to the system log. */
void internal_fatal (const char *file, int line,
                 const char *function, const char *format, ...)
{
	va_list va;
	char *msg;

	va_start (va, format);
	msg = format_msg_va (format, va);
	fprintf (stderr, "\nFATAL_ERROR: %s\n\n", msg);
#ifndef NDEBUG
	internal_logit (file, line, function, "FATAL ERROR: %s", msg);
#endif
	va_end (va);

	log_close ();

	if (im_server)
		syslog (LOG_USER|LOG_ERR, "%s", msg);

	free (msg);

	exit (EXIT_FATAL);
}

void *xmalloc (size_t size)
{
	void *p = malloc(size);

	if (!p) fatal ("Can't allocate memory!");
	return p;
}

void *xcalloc (size_t nmemb, size_t size)
{
	void *p;

	if ((p = calloc(nmemb, size)) == NULL)
		fatal ("Can't allocate memory!");
	return p;
}

void *xrealloc (void *ptr, const size_t size)
{
	void *p;

	if ((p = realloc(ptr, size)) == NULL && size != 0)
		fatal ("Can't allocate memory!");
	return p;
}

char *xstrdup (const char *s)
{
	char *n;

	if (s && (n = strdup(s)) == NULL)
		fatal ("Can't allocate memory!");

	return s ? n : NULL;
}

/* Sleep for the specified number of 'ticks'. */
void xsleep (size_t ticks, size_t ticks_per_sec)
{
	assert(ticks_per_sec > 0);

	if (ticks > 0) {
		int rc;
		struct timespec delay = {.tv_sec = (__time_t)ticks};

		if (ticks_per_sec > 1) {
			uint64_t nsecs;

			delay.tv_sec /= ticks_per_sec;
			nsecs = ticks % ticks_per_sec;

			if (nsecs > 0) {
				assert (nsecs < UINT64_MAX / UINT64_C(1000000000));

				delay.tv_nsec = nsecs * UINT64_C(1000000000);
				delay.tv_nsec /= ticks_per_sec;
			}
		}

		do {
			rc = nanosleep (&delay, &delay);
			if (rc == -1 && errno != EINTR)
				fatal ("nanosleep() failed: %s", xstrerror (errno));
		} while (rc != 0);
	}
}


/* Return error message in malloc() buffer (for strerror_r(3)). */
char *xstrerror (int errnum)
{
	int saved_errno = errno;
	char *err_str, err_buf[256];

#ifdef STRERROR_R_CHAR_P
	/* strerror_r(3) is GNU variant. */
	err_str = strerror_r (errnum, err_buf, sizeof (err_buf));
#else
	/* strerror_r(3) is XSI variant. */
	if (strerror_r (errnum, err_buf, sizeof (err_buf)) < 0) {
		logit ("Error %d occurred obtaining error description for %d",
		        errno, errnum);
		strcpy (err_buf, "Error occurred obtaining error description");
	}
	err_str = err_buf;
#endif

	errno = saved_errno;

	return xstrdup (err_str);
}

/* A signal(2) which is both thread safe and POSIXly well defined. */
void xsignal (int signum, void (*func)(int))
{
	struct sigaction act;

	act.sa_handler = func;
	act.sa_flags = 0;
	sigemptyset (&act.sa_mask);

	if (sigaction(signum, &act, 0) == -1)
		fatal ("sigaction() failed: %s", xstrerror (errno));
}

void set_me_server ()
{
	im_server = 1;
}

/* Extract a substring starting at 'src' for length 'len' and remove
 * any leading and trailing whitespace.  Return NULL if unable.  */
char *trim (const char *src, size_t len)
{
	char *result;
	const char *first, *last;

	for (last = &src[len - 1]; last >= src; last -= 1) {
		if (!isspace (*last))
			break;
	}
	if (last < src)
		return NULL;

	for (first = src; first <= last; first += 1) {
		if (!isspace (*first))
			break;
	}
	if (first > last)
		return NULL;

	last += 1;
	result = (char*) xcalloc (last - first + 1, sizeof (char));
	strncpy (result, first, last - first);
	result[last - first] = 0x00;

	return result;
}

/* Format argument values according to 'format' and return it as a
 * malloc()ed string. */
char *format_msg (const char *format, ...)
{
	char *result;
	va_list va;

	va_start (va, format);
	result = format_msg_va (format, va);
	va_end (va);

	return result;
}

/* Format a vararg list according to 'format' and return it as a
 * malloc()ed string. */
char *format_msg_va (const char *format, va_list va)
{
	int len;
	char *result;
	va_list va_copy;

	va_copy (va_copy, va);
	len = vsnprintf (NULL, 0, format, va_copy) + 1;
	va_end (va_copy);
	result = (char*) xmalloc (len);
	vsnprintf (result, len, format, va);

	return result;
}

int get_realtime (struct timespec *ts)
{
	return clock_gettime (CLOCK_REALTIME, ts);
}

int random_int(int m)
{
	if (m <= 0) return 0;
	int i = (int)(rand() / (float)RAND_MAX * (m+1));
	return i <= m ? i : m;
	// TODO...
}
int random_int(int a, int b)
{
	if (b <= a) return 0;
	int i = a + (int)(rand() / (float)RAND_MAX * (b-a+1));
	return i <= b ? i : b;
	// TODO...

}
