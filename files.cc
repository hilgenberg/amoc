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
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>

#ifdef HAVE_LIBMAGIC
#include <magic.h>
#include <pthread.h>
#endif

#include "lists.h"
#include "client/interface.h"
#include "input/decoder.h"
#include "files.h"
#include "client/utf8.h"
#include "server/ratings.h"

#define READ_LINE_INIT_SIZE	256

#ifdef HAVE_LIBMAGIC
static magic_t cookie = NULL;
static char *cached_file = NULL;
static char *cached_result = NULL;
#endif

void files_init ()
{
#ifdef HAVE_LIBMAGIC
	assert (cookie == NULL);

	cookie = magic_open (MAGIC_SYMLINK | MAGIC_MIME | MAGIC_ERROR |
	                     MAGIC_NO_CHECK_COMPRESS | MAGIC_NO_CHECK_ELF |
	                     MAGIC_NO_CHECK_TAR | MAGIC_NO_CHECK_TOKENS |
	                     MAGIC_NO_CHECK_FORTRAN | MAGIC_NO_CHECK_TROFF);
	if (cookie == NULL)
		log_errno ("Error allocating magic cookie", errno);
	else if (magic_load (cookie, NULL) != 0) {
		logit ("Error loading magic database: %s", magic_error (cookie));
		magic_close (cookie);
		cookie = NULL;
	}
#endif
}

void files_cleanup ()
{
#ifdef HAVE_LIBMAGIC
	free (cached_file);
	cached_file = NULL;
	free (cached_result);
	cached_result = NULL;
	magic_close (cookie);
	cookie = NULL;
#endif
}

/* Is the string a URL? */
int is_url (const char *str)
{
	return !strncasecmp (str, "http://", sizeof ("http://") - 1)
		|| !strncasecmp (str, "ftp://", sizeof ("ftp://") - 1);
}

/* Return 1 if the file is a directory, 0 if not, -1 on error. */
int is_dir (const char *file)
{
	struct stat file_stat;

	if (is_url (file))
		return 0;

	if (stat (file, &file_stat) == -1) {
		char *err = xstrerror (errno);
		error ("Can't stat %s: %s", file, err);
		free (err);
		return -1;
	}
	return S_ISDIR(file_stat.st_mode) ? 1 : 0;
}

int is_plist_file (const char *name)
{
	const char *ext = ext_pos (name);
	return ext && !strcasecmp(ext, "m3u");
}

/* Return 1 if the file can be read by this user, 0 if not */
int can_read_file (const char *file)
{
	return access(file, R_OK) == 0;
}

/* Given a file name, return the mime type or NULL. */
char *file_mime_type (const char *file ASSERT_ONLY)
{
	char *result = NULL;

	assert (file != NULL);

#ifdef HAVE_LIBMAGIC
	static pthread_mutex_t magic_mtx = PTHREAD_MUTEX_INITIALIZER;

	if (cookie != NULL) {
		LOCK(magic_mtx);
		if (cached_file && !strcmp (cached_file, file))
			result = xstrdup (cached_result);
		else {
			free (cached_file);
			free (cached_result);
			cached_file = cached_result = NULL;
			result = xstrdup (magic_file (cookie, file));
			if (result == NULL)
				logit ("Error interrogating file: %s", magic_error (cookie));
			else {
				cached_file = xstrdup (file);
				cached_result = xstrdup (result);
			}
		}
		UNLOCK(magic_mtx);
	}
#endif

	return result;
}

/* Add file to the directory path in buf resolving '../' and removing './'. */
/* buf must be absolute path. */
void resolve_path (char *buf, const int size, const char *file)
{
	int rc;
	char *f; /* points to the char in *file we process */
	char path[2*PATH_MAX]; /* temporary path */
	int len = 0; /* number of characters in the buffer */

	assert (buf[0] == '/');

	rc = snprintf(path, sizeof(path), "%s/%s/", buf, file);
	if (rc >= ssizeof(path))
		fatal ("Path too long!");

	f = path;
	while (*f) {
		if (!strncmp(f, "/../", 4)) {
			char *slash = strrchr (buf, '/');

			assert (slash != NULL);

			if (slash == buf) {

				/* make '/' from '/directory' */
				buf[1] = 0;
				len = 1;
			}
			else {

				/* strip one element */
				*(slash) = 0;
				len -= len - (slash - buf);
				buf[len] = 0;
			}

			f+= 3;
		}
		else if (!strncmp(f, "/./", 3))

			/* skip '/.' */
			f += 2;
		else if (!strncmp(f, "//", 2))

			/* remove double slash */
			f++;
		else if (len == size - 1)
			fatal ("Path too long!");
		else  {
			buf[len++] = *(f++);
			buf[len] = 0;
		}
	}

	/* remove dot from '/dir/.' */
	if (len >= 2 && buf[len-1] == '.' && buf[len-2] == '/')
		buf[--len] = 0;

	/* strip trailing slash */
	if (len > 1 && buf[len-1] == '/')
		buf[--len] = 0;
}

str resolve_path (const str &path)
{
	std::vector<char> buf(path.length()+1);

	int j = 0; // used size of buf
	for (int i = 0; path[i]; ++i)
	{
		if (path[i] == '/')
		{
			// --> single /
			if (j > 0 && buf[j-1] == '/') continue;

			if (j == 1 && buf[0] == '.') // ./ --> empty string
			{
				j = 0;
				while (path[i+1] == '/') ++i;
				continue;
			}

			//  /./ --> /
			if (j >= 2 && buf[j-2] == '/' && buf[j-1] == '.') { --j; continue; }

			//  /something/../ --> /   and   
			if (j >= 3 && buf[j-3] == '/' && buf[j-2] == '.' && buf[j-1] == '.')
			{
				j -= 2;
				if (j == 1) continue; //   /../ --> /
				--j;
				while (j > 0 && buf[j-1] != '/') --j;
				if (j == 0) // foo/../ --> empty string
				{
					while (path[i+1] == '/') ++i;
				}
				continue;
			}
		}

		buf[j++] = path[i];
	}

	if (j >= 2 && buf[j-2] == '/' && buf[j-1] == '.') j -= 2; // remove trailing "/."
	if (j > 1 && buf[j-1] == '/') --j; // remove trailing slash
	buf[j] = 0;
	return str(buf.data());
}

/* Return the file extension position or NULL if the file has no extension. */
const char *ext_pos (const char *file)
{
	const char *ext = strrchr (file, '.');
	const char *slash = strrchr (file, '/');

	/* don't treat dot in ./file or /.file as a dot before extension */
	if (ext && (!slash || slash < ext) && ext != file && *(ext-1) != '/')
		ext++;
	else
		ext = NULL;

	return ext;
}
char *ext_pos (char *file)
{
	char *ext = strrchr (file, '.');
	char *slash = strrchr (file, '/');

	/* don't treat dot in ./file or /.file as a dot before extension */
	if (ext && (!slash || slash < ext) && ext != file && *(ext-1) != '/')
		ext++;
	else
		ext = NULL;

	return ext;
}

/* Read one line from a file, strip trailing end of line chars.
 * Returned memory is malloc()ed.  Return NULL on error or EOF. */
char *read_line (FILE *file)
{
	int line_alloc = READ_LINE_INIT_SIZE;
	int len = 0;
	char *line = (char *)xmalloc (sizeof(char) * line_alloc);

	while (1) {
		if (!fgets(line + len, line_alloc - len, file))
			break;
		len = strlen(line);

		if (line[len-1] == '\n')
			break;

		/* If we are here, it means that line is longer than the buffer. */
		line_alloc *= 2;
		line = (char *)xrealloc (line, sizeof(char) * line_alloc);
	}

	if (len == 0) {
		free (line);
		return NULL;
	}

	if (line[len-1] == '\n')
		line[--len] = 0;
	if (len > 0 && line[len-1] == '\r')
		line[--len] = 0;

	return line;
}

/* Return malloc()ed string in form "base/name". */
static char *add_dir_file (const char *base, const char *name)
{
	char *path;
	int base_is_root;

	base_is_root = !strcmp (base, "/") ? 1 : 0;
	path = (char *)xmalloc (sizeof(char) *
			(strlen(base) + strlen(name) + 2));

	sprintf (path, "%s/%s", base_is_root ? "" : base, name);

	return path;
}

/* Find directories having a prefix of 'pattern'.
 * - If there are no matches, NULL is returned.
 * - If there is one such directory, it is returned with a trailing '/'.
 * - Otherwise the longest common prefix is returned (with no trailing '/').
 * (This is used for directory auto-completion.)
 * Returned memory is malloc()ed.
 * 'pattern' is temporarily modified! */
char *find_match_dir (char *pattern)
{
	char *slash;
	DIR *dir;
	struct dirent *entry;
	int name_len;
	char *name;
	char *matching_dir = NULL;
	char *search_dir;
	int unambiguous = 1;

	if (!pattern[0])
		return NULL;

	/* strip the last directory */
	slash = strrchr (pattern, '/');
	if (!slash)
		return NULL;
	if (slash == pattern) {
		/* only '/dir' */
		search_dir = xstrdup ("/");
	}
	else {
		*slash = 0;
		search_dir = xstrdup (pattern);
		*slash = '/';
	}

	name = slash + 1;
	name_len = strlen (name);

	if (!(dir = opendir(search_dir)))
		return NULL;

	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")
				&& !strncmp(entry->d_name, name, name_len)) {
			char *path = add_dir_file (search_dir, entry->d_name);

			if (is_dir(path) == 1) {
				if (matching_dir) {

					/* More matching directories - strip
					 * matching_dir to the part that is
					 * common to both paths */
					int i = 0;

					while (matching_dir[i] == path[i]
							&& path[i])
						i++;
					matching_dir[i] = 0;
					free (path);
					unambiguous = 0;
				}
				else
					matching_dir = path;
			}
			else
				free (path);
		}
	}

	closedir (dir);
	free (search_dir);

	if (matching_dir && unambiguous) {
		matching_dir = (char *)xrealloc (matching_dir,
				sizeof(char) * (strlen(matching_dir) + 2));
		strcat (matching_dir, "/");
	}

	return matching_dir;
}

/* Return != 0 if the file exists. */
int file_exists (const char *file)
{
	struct stat file_stat;

	if (!stat(file, &file_stat))
		return 1;

	/* Log any error other than non-existence. */
	if (errno != ENOENT)
		log_errno ("Error", errno);

	return 0;
}

/* Get the modification time of a file. Return (time_t)-1 on error */
time_t get_mtime (const char *file)
{
	struct stat stat_buf;

	if (stat(file, &stat_buf) != -1)
		return stat_buf.st_mtime;

	return (time_t)-1;
}

/* Convert file path to absolute path;
 * resulting string is allocated and must be freed afterwards. */
char *absolute_path (const char *path, const char *cwd)
{
	char tmp[2*PATH_MAX];
	char *result;

	assert (path);
	assert (cwd);

	if(path[0] != '/' && !is_url(path)) {
		strncpy (tmp, cwd, sizeof(tmp));
		tmp[sizeof(tmp)-1] = 0;

		resolve_path (tmp, sizeof(tmp), path);

		result = (char *)xmalloc (sizeof(char) * (strlen(tmp)+1));
		strcpy (result, tmp);
	}
	else {
		result = (char *)xmalloc (sizeof(char) * (strlen(path)+1));
		strcpy (result, path);
	}

	return result;
}

/* Check that a file which may cause other applications to be invoked
 * is secure against tampering. */
bool is_secure (const char *file)
{
    struct stat sb;

	assert (file && file[0]);

	if (stat (file, &sb) == -1)
		return true;
	if (!S_ISREG(sb.st_mode))
		return false;
	if (sb.st_mode & (S_IWGRP|S_IWOTH))
		return false;
	if (sb.st_uid != 0 && sb.st_uid != geteuid ())
		return false;

	return true;
}


/* Purge content of a directory. */
bool purge_directory (const char *dir_path)
{
	logit ("Purging %s...", dir_path);

	DIR *dir = opendir (dir_path);
	if (!dir) {
		char *err = xstrerror (errno);
		logit ("Can't open directory %s: %s", dir_path, err);
		free (err);
		return false;
	}

	struct dirent *d;
	while ((d = readdir (dir))) {
		if (!strcmp (d->d_name, ".") || !strcmp (d->d_name, ".."))
			continue;

		int len = strlen (dir_path) + strlen (d->d_name) + 2;
		char *fpath = (char *)xmalloc (len);
		snprintf (fpath, len, "%s/%s", dir_path, d->d_name);

		struct stat st;
		if (stat (fpath, &st) < 0) {
			char *err = xstrerror (errno);
			logit ("Can't stat %s: %s", fpath, err);
			free (err);
			free (fpath);
			closedir (dir);
			return false;
		}

		if (S_ISDIR(st.st_mode)) {
			if (!purge_directory (fpath)) {
				free (fpath);
				closedir (dir);
				return false;
			}

			logit ("Removing directory %s...", fpath);
			if (rmdir (fpath) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath, err);
				free (err);
				free (fpath);
				closedir (dir);
				return false;
			}
		}
		else {
			logit ("Removing file %s...", fpath);

			if (unlink (fpath) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath, err);
				free (err);
				free (fpath);
				closedir (dir);
				return false;
			}
		}

		free (fpath);
	}

	closedir (dir);
	return true;
}
