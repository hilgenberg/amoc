#pragma once

/* store ratings for a file */
bool ratings_write_file (const char *fn, int rating);

/* read ratings for a file */
int ratings_read_file (const char *fn);
