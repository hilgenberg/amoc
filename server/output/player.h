#pragma once

#include "out_buf.h"
#include "../input/io.h"

void player_cleanup ();
void player (const char *file, const char *next_file, struct out_buf *out_buf);
void player_stop ();
void player_seek (const int n);
void player_jump_to (const int n);
void player_reset ();
void player_init ();
void player_pause ();
void player_unpause ();
