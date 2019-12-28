#pragma once

#include <string>
#include <stdexcept>
#include <cassert>

/**
 * Like sprintf for std::string.
 * @param fmt Passed to vsnprintf, so it supports all the usual % escapes.
 * @return The formatted string.
 */
std::string format(const char *fmt, ...);

std::string spaces(int n);

// returns true if s =~ [+-]?\d+
// value is not modified if it returs false, otherwise
// it will be the numeric value of s.
// TODO: Does not handle overflow/underflow
// TODO: There's probably a faster library function for this
bool is_int(const std::string &s, int &value);
bool is_int(const char *s, int &value);

bool has_prefix(const std::string &s, const char *prefix, bool ignore_case);
bool has_prefix(const char *s, const char *prefix, bool ignore_case);
	
