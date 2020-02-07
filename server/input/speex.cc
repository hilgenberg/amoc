/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Based on (and includes code from) ogg123 copyright by
 * Stan Seibert <volsung@xiph.org> AND OTHER CONTRIBUTORS
 * and speexdec copyright by Jean-Marc Valin
 */

#include <inttypes.h>
#include <speex/speex.h>
#include <speex/speex_header.h>
#include <speex/speex_stereo.h>
#include <speex/speex_callbacks.h>
#include <ogg/ogg.h>

#include "decoder.h"
#include "io.h"
#include "../audio.h"

/* Use speex's audio enhancement feature */
#define ENHANCE_AUDIO 1

struct spx_data : public Codec
{
	struct io_stream *stream;
	int ok;			/* was the stream opened succesfully? */

	SpeexBits bits;
	void *st;		/* speex decoder state */
	ogg_sync_state oy;
	ogg_page og;
	ogg_packet op;
	ogg_stream_state os;
	SpeexStereoState stereo;
	SpeexHeader *header;

	int frame_size;
	int rate;
	int nchannels;
	int frames_per_packet;
	int bitrate;

	int16_t *output;
	int output_start;
	int output_left;
	char *comment_packet;
	int comment_packet_len;

	void *process_header ()
	{
		void *st;
		const SpeexMode *mode;
		int modeID;
		SpeexCallback callback;
		int enhance = ENHANCE_AUDIO;

		header = speex_packet_to_header ((char*)op.packet,
				op.bytes);
		if (!header) {
			error.fatal("Can't open speex file: can't read header");
			return NULL;
		}

		if (header->mode >= SPEEX_NB_MODES) {
			error.fatal("Can't open speex file: Mode number %" PRId32
				" does not exist in this version", header->mode);
			return NULL;
		}

		modeID = header->mode;
		mode = speex_mode_list[modeID];

		if (mode->bitstream_version < header->mode_bitstream_version) {
			error.fatal("Can't open speex file: The file was encoded "
				"with a newer version of Speex.");
			return NULL;
		}

		if (mode->bitstream_version > header->mode_bitstream_version) {
			error.fatal("Can't open speex file: The file was encoded "
				"with an older version of Speex.");
			return NULL;
		}

		st = speex_decoder_init (mode);
		speex_decoder_ctl(st, SPEEX_SET_ENH, &enhance);
		speex_decoder_ctl(st, SPEEX_GET_FRAME_SIZE, &frame_size);

		callback.callback_id = SPEEX_INBAND_STEREO;
		callback.func = speex_std_stereo_request_handler;
		callback.data = &stereo;
		speex_decoder_ctl(st, SPEEX_SET_HANDLER, &callback);
		speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &header->rate);

		return st;
	}

	/* Read the speex header. Return 0 on error. */
	int read_speex_header ()
	{
		int packet_count = 0;
		int stream_init = 0;
		char *buf;
		ssize_t nb_read;
		int header_packets = 2;

		while (packet_count < header_packets) {

			/* Get the ogg buffer for writing */
			buf = ogg_sync_buffer (&oy, 200);

			/* Read bitstream from input file */
			nb_read = io_read (stream, buf, 200);

			if (nb_read < 0) {
				error.fatal("Can't open speex file: IO error: %s", io_strerror(stream));
				return 0;
			}

			if (nb_read == 0) {
				error.fatal("Can't open speex header");
				return 0;
			}

			ogg_sync_wrote (&oy, nb_read);

			/* Loop for all complete pages we got (most likely only one) */
			while (ogg_sync_pageout(&oy, &og) == 1) {

				if (stream_init == 0) {
					ogg_stream_init(&os,
							ogg_page_serialno(&og));
					stream_init = 1;
				}

				/* Add page to the bitstream */
				ogg_stream_pagein (&os, &og);

				/* Extract all available packets FIXME: EOS! */
				while (ogg_stream_packetout(&os, &op) == 1) {

					/* If first packet, process as Speex header */
					if (packet_count == 0) {
						st = process_header ();

						if (!st) {
							ogg_stream_clear (&os);
							return 0;
						}

						rate = header->rate;
						nchannels
							= header->nb_channels;
						frames_per_packet
							= header->frames_per_packet;
						/*vbr = header->vbr; */

						if (!frames_per_packet)
							frames_per_packet=1;

						output = (int16_t*)xmalloc (frame_size *
								nchannels *
								frames_per_packet *
								sizeof(int16_t));
						output_start = 0;
						output_left = 0;

						header_packets += header->extra_headers;
					}
					else if (packet_count == 1) {
						comment_packet_len
							= op.bytes;
						comment_packet = (char*) xmalloc (
								sizeof(char) *
								comment_packet_len);
						memcpy (comment_packet,
								op.packet,
								comment_packet_len);
					}

					packet_count++;
				}
			}
		}

		return 1;
	}

	spx_data(io_stream *stream)
	: stream(stream)
	{
		SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;
		st = NULL;
		stereo = stereo;
		header = NULL;
		output = NULL;
		comment_packet = NULL;
		bitrate = -1;
		ogg_sync_init (&oy);
		speex_bits_init (&bits);

		if (!read_speex_header()) {
			ogg_sync_clear (&oy);
			speex_bits_destroy (&bits);
			ok = 0;
		}
		else
			ok = 1;
	}

	~spx_data()
	{
		if (ok) {
			if (st) speex_decoder_destroy (st);
			if (comment_packet) free (comment_packet);
			if (output) free (output);
			speex_bits_destroy (&bits);
			ogg_stream_clear (&os);
			ogg_sync_clear (&oy);
		}

		io_close (stream);
		free (header);
	}

	#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
				((buf[base+2]<<16)&0xff0000)| \
				((buf[base+1]<<8)&0xff00)| \
				(buf[base]&0xff))

	static void parse_comment (const char *str, struct file_tags &tags)
	{
		if (!strncasecmp(str, "title=", strlen ("title=")))
			tags.title = str + strlen ("title=");
		else if (!strncasecmp(str, "artist=", strlen ("artist=")))
			tags.artist = str + strlen ("artist=");
		else if (!strncasecmp(str, "album=", strlen ("album=")))
			tags.album = str + strlen ("album=");
		else if (!strncasecmp(str, "tracknumber=", strlen ("tracknumber=")))
			tags.track = atoi(str + strlen ("tracknumber="));
		else if (!strncasecmp(str, "track=", strlen ("track=")))
			tags.track = atoi(str + strlen ("track="));
	}

	void get_comments (file_tags &tags)
	{
		if (comment_packet && comment_packet_len >= 8) {
			char *c = comment_packet;
			int len, i, nb_fields;
			char *end;
			char *temp = NULL;
			int temp_len = 0;

			/* Parse out vendor string */
			end = c + comment_packet_len;
			len = readint(c, 0);
			c += 4;

			if (c + len > end) {
				logit ("Broken comment");
				return;
			}

			c += len;
			if (c + 4 > end) {
				logit ("Broken comment");
				return;
			}

			nb_fields = readint (c, 0);
			c += 4;

			for (i = 0; i < nb_fields; i++) {
				if (c + 4 > end) {
					if (temp)
						free (temp);
					logit ("Broken comment");
					return;
				}

				len = readint (c, 0);
				c += 4;
				if (c + len > end) {
					logit ("Broken comment");
					if (temp)
						free (temp);
					return;
				}

				if (temp_len < len + 1) {
					temp_len = len + 1;
					if (temp)
						temp = (char*) xrealloc (temp, sizeof(char) *
								temp_len);
					else
						temp = (char*) xmalloc (sizeof(char) *
								temp_len);
				}

				strncpy (temp, c, len);
				temp[len] = '\0';
				debug ("COMMENT: '%s'", temp);
				parse_comment (temp, tags);

				c += len;
			}

			free(temp);
		}
	}

	void get_more_data ()
	{
		char *buf;
		ssize_t nb_read;

		buf = ogg_sync_buffer (&oy, 200);
		nb_read = io_read (stream, buf, 200);
		ogg_sync_wrote (&oy, nb_read);
	}

	int get_duration ()
	{
		ogg_int64_t last_granulepos = 0;

		/* Seek to somewhere near the last page */
		if (io_file_size(stream) > 10000) {
			debug ("Seeking near the end");
			if (io_seek(stream, -10000, SEEK_END) == -1)
				logit ("Seeking failed, scanning whole file");
			ogg_sync_reset (&oy);
		}

		/* Read granulepos from the last packet */
		while (!io_eof(stream)) {

			/* Sync to page and read it */
			while (!io_eof(stream)) {
				if (ogg_sync_pageout(&oy, &og) == 1) {
					debug ("Sync");
					break;
				}

				if (!io_eof(stream)) {
					debug ("Need more data");
					get_more_data ();
				}
			}

			/* We have last packet */
			if (io_eof(stream))
				break;

			last_granulepos = ogg_page_granulepos (&og);
		}

		return last_granulepos / rate;
	}

	int seek (int sec) override
	{
		off_t begin = 0, end, old_pos;

		assert (sec >= 0);

		end = io_file_size (stream);
		if (end == -1)
			return -1;
		old_pos = io_tell (stream);

		debug ("Seek request to %ds", sec);

		while (1) {
			off_t middle = (end + begin) / 2;
			ogg_int64_t granule_pos;
			int position_seconds;

			debug ("Seek to %" PRId64, middle);

			if (io_seek(stream, middle, SEEK_SET) == -1) {
				io_seek (stream, old_pos, SEEK_SET);
				ogg_stream_reset (&os);
				ogg_sync_reset (&oy);
				return -1;
			}

			debug ("Syncing...");

			/* Sync to page and read it */
			ogg_sync_reset (&oy);
			while (!io_eof(stream)) {
				if (ogg_sync_pageout(&oy, &og) == 1) {
					debug ("Sync");
					break;
				}

				if (!io_eof(stream)) {
					debug ("Need more data");
					get_more_data ();
				}
			}

			if (io_eof(stream)) {
				debug ("EOF when syncing");
				return -1;
			}

			granule_pos = ogg_page_granulepos(&og);
			position_seconds = granule_pos / rate;

			debug ("We are at %ds", position_seconds);

			if (position_seconds == sec) {
				ogg_stream_pagein (&os, &og);
				debug ("We have it at granulepos %" PRId64, granule_pos);
				break;
			}
			else if (sec < position_seconds) {
				end = middle;
				debug ("going back");
			}
			else {
				begin = middle;
				debug ("going forward");
			}

			debug ("begin - end %" PRId64 " - %" PRId64, begin, end);

			if (end - begin <= 200) {

				/* Can't find the exact position. */
				sec = position_seconds;
				break;
			}
		}

		ogg_sync_reset (&oy);
		ogg_stream_reset (&os);

		return sec;
	}

	int decode (char *sound_buf, int nbytes, sound_params &sound_params) override
	{
		int bytes_requested = nbytes;
		int16_t *out = (int16_t *)sound_buf;

		sound_params.channels = nchannels;
		sound_params.rate = rate;
		sound_params.fmt = SFMT_S16 | SFMT_NE;

		while (nbytes) {
			int j;

			/* First see if there is anything left in the output buffer and
			* empty it out */
			if (output_left > 0) {
				int to_copy = nbytes / sizeof(int16_t);

				to_copy = MIN(output_left, to_copy);

				memcpy (out, output + output_start,
						to_copy * sizeof(int16_t));

				out += to_copy;
				output_start += to_copy;
				output_left -= to_copy;

				nbytes -= to_copy * sizeof(int16_t);
			}
			else if (ogg_stream_packetout (&os, &op) == 1) {
				int16_t *temp_output = output;

				/* Decode some more samples */

				/* Copy Ogg packet to Speex bitstream */
				speex_bits_read_from (&bits,
						(char*)op.packet, op.bytes);

				for (j = 0; j < frames_per_packet; j++) {

					/* Decode frame */
					speex_decode_int (st, &bits,
							temp_output);
					if (nchannels == 2)
						speex_decode_stereo_int (temp_output,
								frame_size,
								&stereo);

					speex_decoder_ctl (st, SPEEX_GET_BITRATE,
							&bitrate);
					/*samples_decoded += frame_size;*/

					temp_output += frame_size *
						nchannels;
				}

				/*logit ("Read %d bytes from page", frame_size *
						nchannels *
						frames_per_packet);*/

				output_start = 0;
				output_left = frame_size *
					nchannels * frames_per_packet;
			}
			else if (ogg_sync_pageout(&oy, &og) == 1) {

				/* Read in another ogg page */
				ogg_stream_pagein (&os, &og);
				debug ("Granulepos: %" PRId64, ogg_page_granulepos(&og));

			}
			else if (!io_eof(stream)) {
				/* Finally, pull in some more data and try again on the next pass */
				get_more_data ();
			}
			else
				break;
		}

		return bytes_requested - nbytes;
	}

	int get_bitrate () const override
	{
		return bitrate / 1000;
	}

	io_stream* get_stream () override
	{
		return stream;
	}
};

struct spx_decoder : public Decoder
{
	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "spx");
	}

	bool matches_mime (const str &mime) const override
	{
		return !strcasecmp (mime.c_str(), "audio/x-speex")
			|| !strncasecmp (mime.c_str(), "audio/x-speex;", 14)
			|| !strcasecmp (mime.c_str(), "audio/speex")
			|| !strncasecmp (mime.c_str(), "audio/speex;", 12);
	}

	Codec* open(io_stream &stream) override
	{
		return new spx_data(&stream);
	}

	bool can_decode (io_stream &stream) override
	{
		char buf[36];
		return io_peek(&stream, buf, 36) == 36 && !memcmp(buf, "OggS", 4)
			&& !memcmp(buf + 28, "Speex   ", 8);
	}

	void read_tags(const str &file_name, file_tags &tags) override
	{
		auto *data = (spx_data*)Decoder::open(file_name);
		if (data && data->ok) {
			data->get_comments(tags);
			tags.time = data->get_duration();
		}
		delete data;
	}
};

Decoder *speex_plugin ()
{
	return new spx_decoder;
}
