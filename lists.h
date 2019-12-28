#pragma once
#include <string>
#include <vector>

typedef std::vector<std::string> stringlist;

stringlist split(const std::string &s, const char *delims = " \t");

/* Flatten a given string list. The returned memory is a
 * null-terminated list of pointers to the strings copied into
 * memory allocated after the pointer list.  This list is suitable for passing
 * to functions which take such a list as an argument (e.g., execv()).
 * Invoking free() on the returned pointer also frees the strings. */
char** pack(const stringlist &list);

/* Inverse of the pack function. */
stringlist unpack(const char **saved);

