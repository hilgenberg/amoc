#pragma once

size_t strwidth (const str &s);
str    strtail (const str &s, int len);
str    strhead (const str &s, int len);

void   strdel(str &s, int i, int n = 1);
int    strins(str &s, int i, wchar_t c); // returns width of c

void   sanitize(str &s); // replace tabs and such by spaces
str    sanitized(const str &s);
