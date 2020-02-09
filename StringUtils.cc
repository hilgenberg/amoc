#include "StringUtils.h"

#include <cstdarg>
#include <vector>
#include <cassert>
#include <cstring>

str format(const char *fmt, ...)
{
	va_list  ap;
	va_start(ap, fmt);

	char              buf1[1024]; // avoid heap allocation most of the time
	std::vector<char> buf2;       // use this if buf1 is too small
	size_t            size = sizeof(buf1);
	char             *buf  = buf1;
	int               n;
	
	while (true)
	{
		n = vsnprintf(buf, size, fmt, ap);
		if (n >= 0 && (size_t)n <= size) break;
		
		size = n > 0 ? (size_t)n+1 : size*2;
		buf2.resize(size);
		buf = buf2.data();
	}
	
	va_end (ap);
	
	return str(buf, (size_t)n);
}
/* Format a vararg list according to 'format' and return it as a
 * malloc()ed string. */
str format_va(const char *fmt, va_list va)
{
	va_list  ap;
	va_copy (ap, va);

	char              buf1[1024]; // avoid heap allocation most of the time
	std::vector<char> buf2;       // use this if buf1 is too small
	size_t            size = sizeof(buf1);
	char             *buf  = buf1;
	int               n;
	
	while (true)
	{
		n = vsnprintf(buf, size, fmt, ap);
		if (n >= 0 && (size_t)n <= size) break;
		
		size = n > 0 ? (size_t)n+1 : size*2;
		buf2.resize(size);
		buf = buf2.data();
	}
	
	va_end (ap);
	
	return str(buf, (size_t)n);	
}

str spaces(int n)
{
	assert(n >= 0);
	if (n <= 0) return str();
	return str(n, ' ');
}

bool is_int(const char *s, int &v_)
{
	int v = 0, sign = 1;
	if      (*s == '-'){ ++s; sign = -1; }
	else if (*s == '+'){ ++s;            }
	if (!*s) return false;

	for (; *s; ++s)
	{
		char c = *s;
		if (!isdigit(c)) return false;
		v *= 10; v += c - '0';
	}
	v_ = sign * v;
	return true;
}

bool is_int(const str &s, int &v_)
{
	int v = 0, sign = 1;
	size_t i = 0, n = s.length();
	if (!n) return false;
	if      (s[0] == '-'){ ++i; sign = -1; }
	else if (s[0] == '+'){ ++i;            }
	if (i >= n) return false;

	for (; i < n; ++i)
	{
		char c = s[i];
		if (!isdigit(c)) return false;
		v *= 10; v += c - '0';
	}
	v_ = sign * v;
	return true;
}

bool has_prefix(const char *s, const char *p, bool ignore_case)
{
	size_t l = strlen(p);
	if (strlen(s) < l) return false;
	auto cmp = ignore_case ? strncasecmp : strncmp;
	return cmp(s, p, l) == 0;
}
bool has_prefix(const str &s, const char *p, bool ignore_case)
{
	size_t l = strlen(p);
	if (s.length() < l) return false;
	auto cmp = ignore_case ? strncasecmp : strncmp;
	return cmp(s.c_str(), p, l) == 0;
}
bool has_prefix(const str &s, const str &p, bool ignore_case)
{
	size_t l = p.length();
	if (s.length() < l) return false;
	auto cmp = ignore_case ? strncasecmp : strncmp;
	return cmp(s.c_str(), p.c_str(), l) == 0;
}

void intersect(str &s1, const str &s2)
{
	if (s1.empty()) return;
	size_t i = 0, n = std::min(s1.length(), s2.length());
	while (i < n && s1[i]==s2[i]) ++i;
	s1.resize(i);
}

strings split(const str &s, const char *delims)
{
	strings ret;
	size_t i = 0, N = s.length();
	while (i < N)
	{
		size_t j = s.find_first_of(delims, i);
		if (j == i) { ++i; continue; }
		if (j == str::npos)
		{
			ret.emplace_back(s.substr(i));
			break;
		}
		ret.emplace_back(s.substr(i, j-i));
		i = j+1;
	}
	return ret;
}

/* Return a "snapshot" of the given string list.  The returned memory is a
 * null-terminated list of pointers to the given list's strings copied into
 * memory allocated after the pointer list.  This list is suitable for passing
 * to functions which take such a list as an argument (e.g., execv()).
 * Invoking free() on the returned pointer also frees the strings. */
char** pack(const strings &list)
{
	size_t size = (list.size()+1) * sizeof(char*);
	for (auto &s : list) size += s.length() + 1;
	char **result = (char **) xmalloc(size);
	char **p0 = result, *p = (char *) (result + list.size() + 1);
	for (auto &s : list)
	{
		*p0++ = p;
		strcpy (p, s.c_str());
		p += s.length()+ 1;
	}
	*p0 = NULL;
	return result;
}

/* Reload saved strings into a list.  The reloaded strings are appended
 * to the list.  The number of items reloaded is returned. */
strings unpack(const char **saved)
{
	strings ret;
	for (; *saved; ++saved) ret.emplace_back(*saved);
	return ret;
}
