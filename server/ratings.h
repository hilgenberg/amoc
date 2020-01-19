#pragma once

/* store ratings for a file */
bool ratings_write_file (const char *fn, int rating);

/* read ratings for a file. returns 0 if not rated. */
int ratings_read_file (const char *fn);
