#pragma once

#include <string>
#include <stdexcept>
#include <cassert>

// like sprintf for std::string.
str format(const char *fmt, ...);
str format_va(const char *fmt, va_list va);

str spaces(int n);

// returns true if s =~ [+-]?\d+
// value is not modified if it returs false, otherwise
// it will be the numeric value of s.
// TODO: Does not handle overflow/underflow
// TODO: There's probably a faster library function for this
bool is_int(const str &s, int &value);
bool is_int(const char *s, int &value);

bool has_prefix(const str &s, const char *prefix, bool ignore_case);
bool has_prefix(const char *s, const char *prefix, bool ignore_case);
bool has_prefix(const str &s, const str &prefix, bool ignore_case);
	
void intersect(str &s1, const str &s2); // set s1 to their longest common prefix

strings split(const str &s, const char *delims = " \t");

/* Flatten a given string list. The returned memory is a
 * null-terminated list of pointers to the strings copied into
 * memory allocated after the pointer list.  This list is suitable for passing
 * to functions which take such a list as an argument (e.g., execv()).
 * Invoking free() on the returned pointer also frees the strings. */
char** pack(const strings &list);

/* Inverse of the pack function. */
strings unpack(const char **saved);
