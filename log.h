#pragma once

void internal_logit (const char *file, const int line, const char *function, const char *format, ...);
void log_init_stream (FILE *f, const char *fn);
void log_close ();
void interface_error (const char *msg);
void interface_fatal (const char *format, ...);

#ifdef DEBUG
#define debug logit
#else
#define debug(...)  do {} while (0)
#endif

#ifndef NDEBUG

	#define logit(...) internal_logit (__FILE__, __LINE__, __func__, ## __VA_ARGS__)

	#ifndef STRERROR_FN
	#define STRERROR_FN xstrerror
	#endif

	#define log_errno(format, errnum) \
		do { \
			char *err##__LINE__ = STRERROR_FN (errnum); \
			logit (format ": %s", err##__LINE__); \
			free (err##__LINE__); \
		} while (0)

	void log_signal (int sig);

#else
	#define logit(...)  do {} while (0)
	#define log_errno(...) do {} while (0)
	#define log_signal(...) do {} while (0)
#endif

