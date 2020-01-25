#pragma once

/* store ratings for a file */
bool ratings_write (const str &fn, int rating);

/* read ratings for a file. returns 0 if not rated. */
int ratings_read (const str &fn);

/* move ratings after file was renamed and/or moved */
bool ratings_move (const str &old_path, const str &new_path);

/* clear rating after file was deleted, returns old rating (-1 if not found) */
int ratings_remove (const str &old_path);
