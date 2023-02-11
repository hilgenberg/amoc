/*
 * MOC - music on console
 * Copyright (C) 2002 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* This code was based on madlld.c (C) by Bertrand Petit including code
 * from xmms-mad (C) by Sam Clegg and winamp plugin for madlib (C) by
 * Robert Leslie. */

/* FIXME: there can be a bit of silence in mp3 at the end or at the
 * beginning. If you hear gaps between files, it's the file's fault.
 * Can we strip this silence? */

#include <inttypes.h>
#include <mad.h>
#include <id3tag.h>

#include "decoder.h"
#include "io.h"
#include "../audio.h"

#define INPUT_BUFFER (32 * 1024)

struct xing
{
	xing() : flags(0) {}
	long flags;			/* valid fields (see below) */
	unsigned long frames;		/* total number of frames */
	unsigned long bytes;		/* total number of bytes */
	unsigned char toc[100];	/* 100-point seek table */
	long scale;			/* ?? */
};
enum
{
	XING_FRAMES = 1L,
	XING_BYTES  = 2L,
	XING_TOC    = 4L,
	XING_SCALE  = 8L
};

/* parse a Xing VBR header */
static int xing_parse(struct xing *xing, struct mad_bitptr ptr, unsigned int bitlen)
{
	#define XING_MAGIC (('X' << 24) | ('i' << 16) | ('n' << 8) | 'g')
	if (bitlen < 64 || mad_bit_read(&ptr, 32) != XING_MAGIC) goto fail;

	xing->flags = mad_bit_read(&ptr, 32);
	bitlen -= 64;

	if (xing->flags & XING_FRAMES) {
		if (bitlen < 32) goto fail;
		xing->frames = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_BYTES) {
		if (bitlen < 32) goto fail;
		xing->bytes = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_TOC) {
		if (bitlen < 800) goto fail;
		for (int i = 0; i < 100; ++i) xing->toc[i] = mad_bit_read(&ptr, 8);
		bitlen -= 800;
	}

	if (xing->flags & XING_SCALE) {
		if (bitlen < 32) goto fail;
		xing->scale = mad_bit_read(&ptr, 32);
		bitlen -= 32;
	}

	return 0;

	fail:
	xing->flags = 0;
	return -1;
}

static inline int32_t round_sample (mad_fixed_t sample)
{
	sample += 1L << (MAD_F_FRACBITS - 24);
	sample = CLAMP(-MAD_F_ONE, sample, MAD_F_ONE - 1);
	return sample >> (MAD_F_FRACBITS + 1 - 24);
}

static int put_output (char *buf, int buf_len, struct mad_pcm *pcm,
		struct mad_header *header)
{
	unsigned int nsamples;
	mad_fixed_t const *left_ch, *right_ch;
	int olen;

	nsamples = pcm->length;
	left_ch = pcm->samples[0];
	right_ch = pcm->samples[1];
	olen = nsamples * MAD_NCHANNELS (header) * 4;

	if (olen > buf_len) {
		logit ("PCM buffer to small!");
		return 0;
	}

	while (nsamples--) {
		long sample0 = round_sample (*left_ch++);

		buf[0] = 0;
		buf[1] = sample0;
		buf[2] = sample0 >> 8;
		buf[3] = sample0 >> 16;
		buf += 4;

		if (MAD_NCHANNELS(header) == 2) {
			long sample1;

			sample1 = round_sample (*right_ch++);

			buf[0] = 0;
			buf[1] = sample1;
			buf[2] = sample1 >> 8;
			buf[3] = sample1 >> 16;

			buf += 4;
		}
	}

	return olen;
}

struct mp3_data : public Codec
{
	io_stream *io;
	unsigned long bitrate;
	long avg_bitrate;

	unsigned int freq;
	short channels;
	signed long duration;	/* Total time of the file in seconds
	                           (used for seeking). */
	off_t size;				/* Size of the file */

	unsigned char in_buff[INPUT_BUFFER + MAD_BUFFER_GUARD];

	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	int skip_frames; /* how many frames to skip (after seeking) */

	int ok; /* was this stream successfully opened? */

	/* Fill in the mad buffer, return number of bytes read, 0 on eof or error */
	size_t fill_buff ()
	{
		size_t remaining;
		unsigned char *read_start;
		ssize_t read_size;

		if (stream.next_frame != NULL) {
			remaining = stream.bufend - stream.next_frame;
			memmove (in_buff, stream.next_frame, remaining);
			read_start = in_buff + remaining;
			read_size = INPUT_BUFFER - remaining;
		}
		else {
			read_start = in_buff;
			read_size = INPUT_BUFFER;
			remaining = 0;
		}

		read_size = io_read (io, read_start, read_size);
		if (read_size < 0) {
			error.fatal("read error");
			return 0;
		}
		else if (read_size == 0)
			return 0;

		if (io_eof (io)) {
			memset (read_start + read_size, 0, MAD_BUFFER_GUARD);
			read_size += MAD_BUFFER_GUARD;
		}

		mad_stream_buffer(&stream, in_buff, read_size + remaining);
		stream.error = (mad_error)0;

		return read_size;
	}

	int count_time_internal ()
	{
		struct xing xing;
		unsigned long bitrate = 0;
		int has_xing = 0;
		int is_vbr = 0;
		int num_frames = 0;
		mad_timer_t duration = mad_timer_zero;
		struct mad_header header;
		int good_header = 0; /* Have we decoded any header? */

		mad_header_init (&header);

		/* There are three ways of calculating the length of an mp3:
		1) Constant bitrate: One frame can provide the information
			needed: # of frames and duration. Just see how long it
			is and do the division.
		2) Variable bitrate: Xing tag. It provides the number of
			frames. Each frame has the same number of samples, so
			just use that.
		3) All: Count up the frames and duration of each frame
			by decoding each one. We do this if we've no other
			choice, i.e. if it's a VBR file with no Xing tag.
		*/

		while (1) {

			/* Fill the input buffer if needed */
			if (!stream.buffer || stream.error == MAD_ERROR_BUFLEN) {
				if (!fill_buff()) break;
			}

			if (mad_header_decode(&header, &stream) == -1) {
				if (MAD_RECOVERABLE(stream.error)) continue;
				if (stream.error == MAD_ERROR_BUFLEN) continue;
				debug ("Can't decode header: %s", mad_stream_errorstr(&stream));
				break;
			}

			good_header = 1;

			/* Limit xing testing to the first frame header */
			if (!num_frames++ && xing_parse(&xing, stream.anc_ptr, stream.anc_bitlen) != -1)
			{
				is_vbr = 1;

				debug ("Has XING header");

				if ((xing.flags & XING_FRAMES)) {
					has_xing = 1;
					num_frames = xing.frames;
					break;
				}
				debug ("XING header doesn't contain number of frames.");
			}

			/* Test the first n frames to see if this is a VBR file */
			if (!is_vbr && !(num_frames > 20)) {
				if (bitrate && header.bitrate != bitrate) {
					debug ("Detected VBR after %d frames", num_frames);
					is_vbr = 1;
				}
				else
					bitrate = header.bitrate;
			}

			/* We have to assume it's not a VBR file if it hasn't already
			* been marked as one and we've checked n frames for different
			* bitrates */
			else if (!is_vbr) {
				debug ("Fixed rate MP3");
				break;
			}

			mad_timer_add (&duration, header.duration);
		}

		if (!good_header) return -1;

		if (size == -1) { mad_header_finish(&header); return -1; }

		if (!is_vbr) {
			/* time in seconds */
			double time = (size * 8.0) / (header.bitrate);
			double timefrac = (double)time - ((long)(time));

			/* samples per frame */
			long nsamples = 32 * MAD_NSBSAMPLES(&header);

			/* samplerate is a constant */
			num_frames = (long) (time * header.samplerate / nsamples);

			/* the average bitrate is the constant bitrate */
			avg_bitrate = bitrate;

			mad_timer_set(&duration, (long)time, (long)(timefrac*100), 100);
		}

		else if (has_xing) {
			mad_timer_multiply (&header.duration, num_frames);
			duration = header.duration;
		}
		else {
			/* the durations have been added up, and the number of frames
			counted. We do nothing here. */
			debug ("Counted duration by counting frames durations in VBR file.");
		}

		if (avg_bitrate == -1 && mad_timer_count(duration, MAD_UNITS_SECONDS) > 0) {
			avg_bitrate = size / mad_timer_count(duration, MAD_UNITS_SECONDS) * 8;
		}

		mad_header_finish(&header);

		debug ("MP3 time: %ld", mad_timer_count (duration, MAD_UNITS_SECONDS));

		return mad_timer_count (duration, MAD_UNITS_SECONDS);
	}

	mp3_data(const char *file)
	{
		ok = 0;
		freq = 0;
		channels = 0;
		skip_frames = 0;
		bitrate = -1;
		avg_bitrate = -1;
		try {
			io = new io_stream(file);
		}
		catch (std::exception &e)
		{
			error.fatal("Can't open: %s", e.what());
		}
		ok = 1;

		size = io_file_size (io);

		mad_stream_init (&stream);
		mad_frame_init (&frame);
		mad_synth_init (&synth);

		mad_stream_options (&stream, MAD_OPTION_IGNORECRC);

		duration = count_time_internal ();
		mad_frame_mute (&frame);
		stream.next_frame = NULL;
		stream.sync = 0;
		stream.error = MAD_ERROR_NONE;

		if (io_seek(io, 0, SEEK_SET) == -1) {
			error.fatal("seek failed");
			mad_stream_finish (&stream);
			mad_frame_finish (&frame);
			mad_synth_finish (&synth);
			ok = 0;
		}

		stream.error = MAD_ERROR_BUFLEN;
	}

	mp3_data(io_stream *s)
	{
		ok = 1;
		freq = 0;
		channels = 0;
		skip_frames = 0;
		bitrate = -1;
		io = s;
		duration = -1;
		size = -1;

		mad_stream_init (&stream);
		mad_frame_init (&frame);
		mad_synth_init (&synth);

		mad_stream_options (&stream, MAD_OPTION_IGNORECRC);
	}

	~mp3_data()
	{
		if (ok) {
			mad_stream_finish (&stream);
			mad_frame_finish (&frame);
			mad_synth_finish (&synth);
		}
		delete io;
	}

	/* If the current frame in the stream is an ID3 tag, then swallow it. */
	ssize_t flush_id3_tag ()
	{
		size_t remaining = stream.bufend - stream.next_frame;
		ssize_t tag_size = id3_tag_query (stream.this_frame, remaining);
		if (tag_size > 0) {
			mad_stream_skip (&stream, tag_size);
			mad_stream_sync (&stream);
		}
		return tag_size;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		error.clear();

		while (1) {

			/* Fill the input buffer if needed */
			if (stream.buffer == NULL ||
				stream.error == MAD_ERROR_BUFLEN) {
				if (!fill_buff())
					return 0;
			}

			if (mad_frame_decode (&frame, &stream)) {
				if (flush_id3_tag ())
					continue;
				else if (MAD_RECOVERABLE(stream.error)) {

					/* Ignore LOSTSYNC */
					if (stream.error == MAD_ERROR_LOSTSYNC)
						continue;

					if (!skip_frames)
						error.warn("Broken frame: %s",
							mad_stream_errorstr(&stream));
					continue;
				}
				else if (stream.error == MAD_ERROR_BUFLEN)
					continue;
				else {
					error.fatal(
							"Broken frame: %s",
							mad_stream_errorstr(&stream));
					return 0;
				}
			}

			if (skip_frames) {
				skip_frames--;
				continue;
			}

			/* Sound parameters. */
			if (!(sound_params.rate = frame.header.samplerate)) {
				error.fatal("Broken file: information about the frequency couldn't be read.");
				return 0;
			}

			sound_params.channels = MAD_NCHANNELS(&frame.header);
			sound_params.fmt = SFMT_S32 | SFMT_LE;

			/* Change of the bitrate? */
			if (frame.header.bitrate != bitrate) {
				if ((bitrate = frame.header.bitrate) == 0) {
					error.fatal("Broken file: information about the bitrate couldn't be read.");
					return 0;
				}
			}

			mad_synth_frame (&synth, &frame);
			mad_stream_sync (&stream);

			return put_output (buf, buf_len, &synth.pcm, &frame.header);
		}
	}

	int seek (int sec) override
	{
		assert (sec >= 0);
		if (size == -1) return -1;
		if (sec >= duration) return -1;

		off_t new_position = ((double) sec / (double) duration) * size;

		debug ("Seeking to %d (byte %" PRId64 ")", sec, new_position);

		if (new_position < 0)
			new_position = 0;
		else if (new_position >= size)
			return -1;

		if (io_seek(io, new_position, SEEK_SET) == -1) {
			logit ("seek to %" PRId64 " failed", new_position);
			return -1;
		}

		stream.error = MAD_ERROR_BUFLEN;

		mad_frame_mute (&frame);
		mad_synth_mute (&synth);

		stream.sync = 0;
		stream.next_frame = NULL;

		skip_frames = 2;

		return sec;
	}

	int get_bitrate () const override
	{
		return bitrate / 1000;
	}

	int get_avg_bitrate () const override
	{
		return avg_bitrate / 1000;
	}

	int get_duration () const override
	{
		return duration;
	}

	struct io_stream *mp3_get_stream ()
	{
		return io;
	}
};

struct mp3_decoder : public Decoder
{
	Codec* open (const str &file) override
	{
		return new mp3_data(file.c_str());
	}

	/* Get the time for mp3 file, return -1 on error.
	* Adapted from mpg321. */
	int get_duration(const str &file) override
	{
		mp3_data data(file.c_str());
		return data.ok ? data.duration : -1;
	}

	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "mp3")
			|| !strcasecmp (ext, "mpga")
			|| !strcasecmp (ext, "mp2")
			|| !strcasecmp (ext, "mp1");
	}

	bool matches_mime (const str &mime) const override
	{
		return !strcasecmp (mime.c_str(), "audio/mpeg")
			|| !strncasecmp (mime.c_str(), "audio/mpeg;", 11);
	}

	bool can_decode (io_stream &stream) override
	{
		unsigned char buf[16 * 1024];

		/* We must use such a sophisticated test, because there are Shoutcast
		* servers that can start broadcasting in the middle of a frame, so we
		* can't use any fewer bytes for magic values. */
		if (io_peek(&stream, buf, sizeof(buf)) == sizeof(buf)) {
			struct mad_stream stream;
			struct mad_header header;
			int dec_res;

			mad_stream_init (&stream);
			mad_header_init (&header);

			mad_stream_buffer (&stream, buf, sizeof(buf));
			stream.error = (mad_error)0;

			while ((dec_res = mad_header_decode(&header, &stream)) == -1
					&& MAD_RECOVERABLE(stream.error))
				;

			return dec_res != -1;
		}

		return false;
	}
};

Decoder *mp3_plugin()
{
	return new mp3_decoder;
}
