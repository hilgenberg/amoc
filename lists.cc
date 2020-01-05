#include "lists.h"

typedef std::vector<std::string> stringlist;

stringlist split(const std::string &s, const char *delims)
{
	stringlist ret;
	size_t i = 0, N = s.length();
	while (i < N)
	{
		size_t j = s.find_first_of(delims, i);
		if (j == i) { ++i; continue; }
		if (j == std::string::npos)
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
char** pack(const stringlist &list)
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
stringlist unpack(const char **saved)
{
	stringlist ret;
	for (; *saved; ++saved) ret.emplace_back(*saved);
	return ret;
}

