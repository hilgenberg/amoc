#ifndef SERVER_H
#define SERVER_H

#include "../playlist.h"

#define CLIENTS_MAX 3

enum PlayState : int
{
	STATE_PLAY,
	STATE_STOP,
	STATE_PAUSE
};

void server_init (int debug, int foreground);
void server_loop ();
void server_error (const char *file, int line, const char *function, const char *msg);
void state_change ();
void set_info_rate (const int rate);
void set_info_channels (const int channels);
void set_info_bitrate (const int bitrate);
void set_info_avg_bitrate (const int avg_bitrate);
void tags_change ();
void ctime_change ();
void status_msg (const str &msg);
void tags_response (const int client_id, const str &file, const file_tags *tags);

#endif
