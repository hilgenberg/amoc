/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include "common.h"
#include "lists.h"
#include "log.h"
#include "options.h"

#ifndef NDEBUG
static FILE *logfp = NULL; /* logging file stream */

static enum {
	UNINITIALISED,
	BUFFERING,
	LOGGING
} logging_state = UNINITIALISED;

static stringlist buffered_log;
static int log_records_spilt = 0;

static pthread_mutex_t logging_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct {
	int sig;
	const char *name;
	volatile uint64_t raised;
	uint64_t logged;
} sig_info[] = {
	{SIGINT, "SIGINT", 0, 0},
	{SIGHUP, "SIGHUP", 0, 0},
	{SIGQUIT, "SIGQUIT", 0, 0},
	{SIGTERM, "SIGTERM", 0, 0},
	{SIGCHLD, "SIGCHLD", 0, 0},
#ifdef SIGWINCH
	{SIGWINCH, "SIGWINCH", 0, 0},
#endif
	{0, "SIG other", 0, 0}
};
#endif

#ifndef NDEBUG
void log_signal (int sig)
{
	int ix = 0;

	while (sig_info[ix].sig && sig_info[ix].sig != sig)
		ix += 1;

	sig_info[ix].raised += 1;
}
#endif

#ifndef NDEBUG
static inline void flush_log (void)
{
	int rc;

	if (logfp) {
		do {
			rc = fflush (logfp);
		} while (rc != 0 && errno == EINTR);
	}
}
#endif

#ifndef NDEBUG
static void locked_logit (const char *file, const int line,
                          const char *function, const char *msg)
{
	char time_str[20];
	struct timespec utc_time;
	time_t tv_sec;
	struct tm tm_time;
	const char fmt[] = "%s.%06ld: %s:%d %s(): %s\n";

	assert (logging_state == BUFFERING || logging_state == LOGGING);
	assert (logging_state != BUFFERING || !logfp);

	if (logging_state == LOGGING && !logfp)
		return;

	get_realtime (&utc_time);
	tv_sec = utc_time.tv_sec;
	localtime_r (&tv_sec, &tm_time);
	strftime (time_str, sizeof (time_str), "%b %e %T", &tm_time);

	if (logfp) {
		
		fprintf (logfp, "%s:%d %s(): %s\n", 
		                     file, line, function, msg);

		//fprintf (logfp, fmt, time_str, utc_time.tv_nsec / 1000L, file, line, function, msg);
	}
	else if (logging_state == BUFFERING)
		buffered_log.push_back(
			format(fmt, time_str, utc_time.tv_nsec / 1000L, file, line, function, msg));
}
#endif

#ifndef NDEBUG
static void log_signals_raised (void)
{
	size_t ix;

    for (ix = 0; ix < ARRAY_SIZE(sig_info); ix += 1) {
		while (sig_info[ix].raised > sig_info[ix].logged) {
			locked_logit (__FILE__, __LINE__, __func__, sig_info[ix].name);
			sig_info[ix].logged += 1;
		}
	}
}
#endif

/* Put something into the log.  If built with logging disabled,
 * this function is provided as a stub so independant plug-ins
 * configured with logging enabled can still resolve it. */
void internal_logit (const char *file LOGIT_ONLY,
                     const int line LOGIT_ONLY,
                     const char *function LOGIT_ONLY,
                     const char *format LOGIT_ONLY, ...)
{
#ifndef NDEBUG
	int saved_errno = errno;
	char *msg;
	va_list va;

	LOCK(logging_mtx);

	if (!logfp) {
		switch (logging_state) {
		case UNINITIALISED:
			logging_state = BUFFERING;
			break;
		case BUFFERING:
			/* Don't let storage run away on us. */
			if (buffered_log.size() < 128)
				break;
			++log_records_spilt;
		case LOGGING:
			goto end;
		}
	}

	log_signals_raised ();

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);
	locked_logit (file, line, function, msg);
	free (msg);

	flush_log ();

	log_signals_raised ();

end:
	UNLOCK(logging_mtx);

	errno = saved_errno;
#endif
}

/* Initialize logging stream */
void log_init_stream (FILE *f LOGIT_ONLY, const char *fn LOGIT_ONLY)
{
#ifndef NDEBUG
	char *msg;

	LOCK(logging_mtx);

	logfp = f;

	if (logging_state == BUFFERING) {
		if (logfp) {
			for (auto &s : buffered_log)
				fprintf (logfp, "%s", s.c_str());
		}
		buffered_log.clear();
	}

	logging_state = LOGGING;
	if (!logfp)
		goto end;

	msg = format_msg ("Writing log to: %s", fn);
	locked_logit (__FILE__, __LINE__, __func__, msg);
	free (msg);

	if (log_records_spilt > 0) {
		msg = format_msg ("%d log records spilt", log_records_spilt);
		locked_logit (__FILE__, __LINE__, __func__, msg);
		free (msg);
	}

	flush_log ();

end:
	UNLOCK(logging_mtx);
#endif
}

void log_close ()
{
#ifndef NDEBUG
	LOCK(logging_mtx);

	if (!(logfp == stdout || logfp == stderr || logfp == NULL)) {
		fclose (logfp);
		logfp = NULL;
	}

	buffered_log.clear();
	log_records_spilt = 0;

	UNLOCK(logging_mtx);
#endif
}
