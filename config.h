#define HAVE_ALSA 1
#define HAVE_JACK 1
#define HAVE_OSS 1
#define HAVE_SNDIO 1

#define HAVE_MMAP 1 /* Define to 1 if you have a working `mmap' system call. */
/* #undef HAVE_TREMOR */ /* Define if you have integer Vorbis. */

#define PACKAGE_NAME      "AMOC"
#define PACKAGE_VERSION   "2.6-alpha3"
#define PACKAGE_URL       "http://github.com/hilgenberg/amoc/"
#define PACKAGE_BUGREPORT "th@zoon.cc"

/* Use 64bit IO */
#define _FILE_OFFSET_BITS 64

#define STRERROR_R_CHAR_P

#include <vector>
#include <queue>
#include <set>
#include <map>
#include <string>
#include <algorithm>
#include <limits>
#include <cmath>

#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <locale.h>
#include <ctype.h>

typedef std::string str;
typedef std::pair<int,int> Ratio;

#include "common.h"
#include "log.h"
#include "options.h"
#include "StringFormatting.h"
#include "lists.h"
#include "files.h"
