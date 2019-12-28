#pragma once
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <byteswap.h>
#include <algorithm>

struct timespec;

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
# define ATTR_PRINTF(x,y) __attribute__ ((format (printf, x, y)))
#else
# define ATTR_PRINTF(...)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_NORETURN
# define ATTR_NORETURN __attribute__((noreturn))
#else
# define ATTR_NORETURN
#endif

#ifdef HAVE_VAR_ATTRIBUTE_UNUSED
# define ATTR_UNUSED __attribute__((unused))
#else
# define ATTR_UNUSED
#endif

#define CONFIG_DIR      ".moc"
#define LOCK(mutex)     pthread_mutex_lock (&mutex)
#define UNLOCK(mutex)   pthread_mutex_unlock (&mutex)
#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(x[0]))
#define ssizeof(x)      ((ssize_t) sizeof(x))

/* Maximal string length sent/received. */
#define MAX_SEND_STRING	4096

/* Exit status on fatal error. */
#define EXIT_FATAL	2

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef LIMIT
#define LIMIT(val, lim) ((val) >= 0 && (val) < (lim))
#endif

#ifndef RANGE
#define RANGE(min, val, max) ((val) >= (min) && (val) <= (max))
#endif

#ifndef CLAMP
#define CLAMP(min, val, max) ((val) < (min) ? (min) : \
                              (val) > (max) ? (max) : (val))
#endif

#ifdef NDEBUG
#define error(...) \
	internal_error (NULL, 0, NULL, ## __VA_ARGS__)
#define fatal(...) \
	internal_fatal (NULL, 0, NULL, ## __VA_ARGS__)
#define ASSERT_ONLY ATTR_UNUSED
#else
#define error(...) \
	internal_error (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define fatal(...) \
	internal_fatal (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define ASSERT_ONLY
#endif

#ifndef STRERROR_FN
# define STRERROR_FN xstrerror
#endif

#define error_errno(format, errnum) \
	do { \
		char *err##__LINE__ = STRERROR_FN (errnum); \
		error (format ": %s", err##__LINE__); \
		free (err##__LINE__); \
	} while (0)

void *xmalloc (size_t size);
void *xcalloc (size_t nmemb, size_t size);
void *xrealloc (void *ptr, const size_t size);
char *xstrdup (const char *s);
void xsleep (size_t ticks, size_t ticks_per_sec);
char *xstrerror (int errnum);
void xsignal (int signum, void (*func)(int));

void internal_error (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_PRINTF(4, 5);
void internal_fatal (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_NORETURN ATTR_PRINTF(4, 5);
void set_me_server ();
char *str_repl (char *target, const char *oldstr, const char *newstr);
char *trim (const char *src, size_t len);
char *format_msg (const char *format, ...);
char *format_msg_va (const char *format, va_list va);
bool is_valid_symbol (const char *candidate);
char *create_file_name (const char *file);
int get_realtime (struct timespec *ts);
void sec_to_min (char *buff, const int seconds);
const char *get_home ();
void common_cleanup ();



#ifndef SUN_LEN
#define SUN_LEN(p) \
        ((sizeof *(p)) - sizeof((p)->sun_path) + strlen ((p)->sun_path))
#endif

/* Maximum path length, we don't consider exceptions like mounted NFS */
#ifndef PATH_MAX
# if defined(_POSIX_PATH_MAX)
#  define PATH_MAX	_POSIX_PATH_MAX /* Posix */
# elif defined(MAXPATHLEN)
#  define PATH_MAX	MAXPATHLEN      /* Solaris? Also linux...*/
# else
#  define PATH_MAX	4096             /* Suppose, we have 4096 */
# endif
#endif

#if !HAVE_DECL_STRCASESTR && !defined(__cplusplus)
char *strcasestr (const char *haystack, const char *needle);
#endif

#include "StringFormatting.h"
#include "lists.h"

