#pragma once

int random_int(int max); // between 0 and max, both inclusive
int random_int(int min, int max); // between min and max, both inclusive

struct timespec;

#define ATTR_NORETURN __attribute__((noreturn))

#define LOCK_(mutex)     pthread_mutex_lock (&mutex)
#define UNLOCK_(mutex)   pthread_mutex_unlock (&mutex)
#if 0
#define LOCK(mutex)     do{ logit("}}} Locking " #mutex); pthread_mutex_lock (&mutex);}while(0)
#define UNLOCK(mutex)   do{ logit("{{{ Unlocking " #mutex); pthread_mutex_unlock (&mutex);}while(0)
#else
#define LOCK(mutex)     pthread_mutex_lock (&mutex)
#define UNLOCK(mutex)   pthread_mutex_unlock (&mutex)
#endif
#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(x[0]))
#define ssizeof(x)      ((ssize_t) sizeof(x))

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
#define error(...) internal_error (NULL, 0, NULL, ## __VA_ARGS__)
#define fatal(...) internal_fatal (NULL, 0, NULL, ## __VA_ARGS__)
#else
#define error(...) internal_error (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define fatal(...) internal_fatal (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
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
                     const char *format, ...);
void internal_fatal (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_NORETURN;
void set_me_server ();
char *trim (const char *src, size_t len);
char *format_msg (const char *format, ...);
char *format_msg_va (const char *format, va_list va);
int get_realtime (struct timespec *ts);
double now();

#ifndef SUN_LEN
#define SUN_LEN(p) \
        ((sizeof *(p)) - sizeof((p)->sun_path) + strlen ((p)->sun_path))
#endif
