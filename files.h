#pragma once

void files_init ();
void files_cleanup ();

str  add_path(const str &p1, const str &p2); // like (cd p1; cd p2; return pwd) p1,p2 assumed to be normalized!
str& normalize_path(str &p); // modifies and returns p
str  normalized_path(const str &p);
str  absolute_path(const str &p);

const char *ext_pos (const char *file);
char *ext_pos (char *file);
char *create_file_name (const char *file);
const char *get_home ();

int file_exists (const char *file);
int is_dir (const char *file);
int is_sound_file (const char *name); // in decoder.cc
int is_url (const char *str);
int is_plist_file (const char *name);

time_t get_mtime (const char *file);
int can_read_file (const char *file);
bool is_secure (const char *file);
char *file_mime_type (const char *file);

char *read_line (FILE *file);
char *find_match_dir (char *dir);

bool purge_directory (const char *dir_path);


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
