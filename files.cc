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

#include <sys/stat.h>
#include <dirent.h>
#include <magic.h>
#include <pthread.h>

#include "client/interface.h"
#include "server/input/decoder.h"
#include "files.h"
#include "client/Util/utf8.h"
#include "server/ratings.h"

#define READ_LINE_INIT_SIZE	256

static magic_t cookie = NULL;
static char *cached_file = NULL;
static char *cached_result = NULL;

void files_init ()
{
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
}

void files_cleanup ()
{
	free (cached_file);
	cached_file = NULL;
	free (cached_result);
	cached_result = NULL;
	magic_close (cookie);
	cookie = NULL;
}

str add_path(const str &p1, const str &p2)
{
	assert(p1 == normalized_path(p1));
	assert(p2 == normalized_path(p2));
	
	if (p2.empty() || p2 == "." || p2 == "./") return p1;
	if (p2.front() == '/') return p2;
	if (p1.empty() || p1 == ".") return p2;
	
	if (p2 == ".." || p2 == "../")
	{
		if (p1 == "/") return p1;  /*  /.. == /  */
		if (p1 == ".") return "..";/*  ./.. == ..  */
		if (p1.length() <= 1) return str();/*  a/.. == ""  */
		auto i = p1.rfind('/', p1.length()-2);
		if (i == str::npos) return str(); /*  foo/.. == ""  */
		if (i == 0) return "/";/*  /foo/.. == /  */
		return p1.substr(0, i);/*  foo/bar/.. == foo  */
	}

	if (has_prefix(p2, "../", false))
	{
		str p = p1;
		if (p1.back() != '/') p += '/';
		p += p2;
		return normalize_path(p);
	}

	if (p1.back() != '/') return p1 + "/" + p2;
	return p1 + p2;
}
str normalized_path(const str &p)
{
	str tmp(p);
	return normalize_path(tmp);
}

str& normalize_path(str &p)
{
	size_t n = p.length();
	if (!n) return p;

	if (p[0] == '~' && (n == 1 || p[1] == '/'))
	{
		p.replace(0, 1, options::Home);
		n = p.length();
	}

	// foo//././//bar//./  --> foo/bar
	size_t j = 0;
	for (size_t i = 0; i < n; ++i)
	{
		if (p[i] == '/')
		{
			while (i < n)
			{
				if (i+1 < n && p[i+1] == '/') { ++i; continue; }
				if (i+2 < n && p[i+1] == '.' && p[i+2] == '/') { i += 2; continue; }
				if (i+2 == n && p[i+1] == '.') { i += 2; break; }
				if (i+1 == n) { ++i; break; }
				break;
			}
			if (i >= n) break;
		}
		p[j++] = p[i];
	}
	if (j == 0) return p = "/";
	p.resize(n = j);

	//  /../../..foo  --> /..foo,  /../.. --> /
	if (p[0] == '/')
	{
		j = 0;
		while (j+2 < n && p[j+1] == '.' && p[j+2] == '.')
		{
			if (j+3 == n) return p = "/";
			if (j+3 < n && p[j+3] == '/') { j += 3; continue; }
			if (j > 0) p = p.substr(j); break;
		}
	}

	// ../../foo/bar/../a/b/c/../..  --> ../../foo/a

	j = n; // start of the final part we know we can keep  (except initial slash maybe)
	       // (and foo/../bar is the case where the initial slash will need to be removed)
	while (j > 0)
	{
		// invariant: p[j-1] is never a slash but j == n or p[j] == '/'
		size_t i = j, up = 0;
		while (i > 3 && p[i-3] == '/' && p[i-2] == '.' && p[i-1] == '.') { i -= 3; ++up; }

		if (!up)
		{
			// we can keep the last part before j too
			while (j > 0 && p[j-1] != '/') --j;
			if (j > 0) --j; // keep the slash too
			continue;
		}
		if (i == 2 && p[0] == '.' && p[1] == '.') break; // done

		while (up)
		{
			// check for and return ../../good_part
			if (i == 2 && p[0] == '.' && p[1] == '.') return p;
			
			// remove the middle /foo/..

			// p[i] is the initial slash in some /../../good_part
			// we know the part before i is not /.. because up is maximal
			// we know i is not 0 because initial /.. were handled already
			size_t k = i-1;
			while (k > 0 && p[k] != '/') --k;

			// now k==0 or p[k] is the initial slash in /foo/../../good_part, which
			// can become /../good_part with up -= 1;
			// if k==0, we have foo/../../good_part, which should become ../good_part,
			// which we can return
			if (p[k] != '/') // (k == 0 is not enough!)
			{
				if (up == 1 && j == n) // foo/..  --> empty
					return p = "";

				// foo/../..  --> ..
				// foo/../../bar --> ../bar
				// foo/../bar --> bar
				return p = p.substr(i+4);
			}

			// prefix/foo/../good_part --> prefix/good_part
			if (j == n && k == 0) return p = "/"; //  /foo/.. --> /
			size_t d = 1/*slash at k*/ + i-k-1 /*strlen(foo)*/ + 3 /*../*/;
			p = p.replace(k, d, "");
			j -= d; n -= d;
			--up; // and if up > 0, then j > 0
		}
	}
	return p;
}

str absolute_path(const str &p)
{
	size_t n = p.length();
	if (n && p[0] == '/') return p;

	if (n && p[0] == '~' && (n == 1 || p[1] == '/'))
	{
		str q(p);
		return q.replace(0, 1, options::Home);
	}

	char *cwd = getcwd(NULL,0);
	if (!cwd) interface_fatal ("Can't get CWD: %s", xstrerror (errno));
	auto ret = add_path(cwd, p);
	free(cwd);
	return ret;
}

str containing_directory(const str &path)
{
	assert(!path.empty() && path[0] == '/');
	if (path == "/") return path;
	auto i = path.rfind('/');
	if (i == str::npos) return "";
	return path.substr(0, i==0 ? (size_t)1 : i);
}
str file_name(const str &path)
{
	assert(!path.empty() && path.back() != '/');
	auto i = path.rfind('/', path.length()<2 ? str::npos : path.length()-2);
	return i == str::npos ? path : path.substr(i+1);
}


/* Is the string a URL? */
bool is_url (const str &str)
{
	return has_prefix(str, "http://", true) || has_prefix(str, "https://", true) ||
	       has_prefix(str, "ftp://", true);
}

/* Return 1 if the file is a directory, 0 if not, -1 on error. */
bool is_dir (const str &file)
{
	if (file.empty() || is_url(file)) return false;

	struct stat file_stat;
	if (stat (file.c_str(), &file_stat) == -1) {
		if (errno == ENOENT) return false;
		char *err = xstrerror (errno);
		error ("Can't stat %s: %s", file.c_str(), err);
		free (err);
		return false;
	}
	return S_ISDIR(file_stat.st_mode);
}

bool file_exists (const str &file)
{
	struct stat file_stat;

	if (!stat(file.c_str(), &file_stat))
		return 1;

	/* Log any error other than non-existence. */
	if (errno != ENOENT)
		log_errno ("Error", errno);

	return 0;
}

bool is_regular_file(const str &file)
{
	struct stat file_stat;
	if (stat(file.c_str(), &file_stat) != 0) return false;
	return S_ISREG(file_stat.st_mode);
}

bool is_plist_file (const str &name)
{
	const char *ext = ext_pos (name.c_str());
	return ext && !strcasecmp(ext, "m3u");
}

/* Return 1 if the file can be read by this user, 0 if not */
bool can_read_file (const str &file)
{
	return access(file.c_str(), R_OK) == 0;
}

/* Given a file name, return the mime type or NULL. */
char *file_mime_type (const char *file)
{
	char *result = NULL;

	assert (file != NULL);

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

	return result;
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

/* Get the modification time of a file. Return (time_t)-1 on error */
time_t get_mtime (const str &file)
{
	struct stat stat_buf;

	if (stat(file.c_str(), &stat_buf) != -1)
		return stat_buf.st_mtime;

	return (time_t)-1;
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
bool purge_directory (const str &dir_path)
{
	logit ("Purging %s...", dir_path.c_str());

	DIR *dir = opendir (dir_path.c_str());
	if (!dir) {
		char *err = xstrerror (errno);
		logit ("Can't open directory %s: %s", dir_path.c_str(), err);
		free (err);
		return false;
	}

	struct dirent *d;
	while ((d = readdir(dir)))
	{
		if (!strcmp(d->d_name, ".") || !strcmp (d->d_name, "..")) continue;
		str fpath = add_path(dir_path, d->d_name);

		struct stat st;
		if (stat (fpath.c_str(), &st) < 0) {
			if (errno == ENOENT) return false;

			char *err = xstrerror (errno);
			logit ("Can't stat %s: %s", fpath.c_str(), err);
			free (err);
			closedir (dir);
			return false;
		}

		if (S_ISDIR(st.st_mode)) {
			if (!purge_directory (fpath)) {
				closedir (dir);
				return false;
			}

			logit ("Removing directory %s...", fpath.c_str());
			if (rmdir (fpath.c_str()) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath.c_str(), err);
				free (err);
				closedir (dir);
				return false;
			}
		}
		else {
			logit ("Removing file %s...", fpath.c_str());

			if (unlink (fpath.c_str()) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath.c_str(), err);
				free (err);
				closedir (dir);
				return false;
			}
		}
	}

	closedir (dir);
	return true;
}
