#pragma once
#include <stdio.h>

void files_init ();
void files_cleanup ();

str resolve_path (const str &path);
void resolve_path (char *buf, const int size, const char *file);
char *absolute_path (const char *path, const char *cwd);
const char *ext_pos (const char *file);
char *ext_pos (char *file);

int file_exists (const char *file);
int is_dir (const char *file);
time_t get_mtime (const char *file);
int can_read_file (const char *file);
bool is_secure (const char *file);
char *file_mime_type (const char *file);
int is_url (const char *str);
int is_plist_file (const char *name);

char *read_line (FILE *file);
char *find_match_dir (char *dir);

bool purge_directory (const char *dir_path);
