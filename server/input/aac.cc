/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This code is based on CMUS aac plugin Copyright 2006 dnk <dnk@bjum.net>
 *
 */

#include <neaacdec.h>
#include <id3tag.h>

#include "decoder.h"
#include "io.h"

/* FAAD_MIN_STREAMSIZE == 768, 6 == # of channels */
#define BUFFER_SIZE	(FAAD_MIN_STREAMSIZE * 6 * 4)

/* 'data' must point to at least 6 bytes of data */
static int parse_frame (const unsigned char data[6])
{
	int len;

	/* http://wiki.multimedia.cx/index.php?title=ADTS */

	/* first 12 bits must be set */
	if (data[0] != 0xFF) return 0;
	if ((data[1] & 0xF0) != 0xF0) return 0;

	/* layer is always '00' */
	if ((data[1] & 0x06) != 0x00) return 0;

	/* frame length is stored in 13 bits */
	len  = data[3] << 11;	/* ..1100000000000 */
	len |= data[4] << 3;	/* ..xx11111111xxx */
	len |= data[5] >> 5;	/* ..xxxxxxxxxx111 */
	len &= 0x1FFF;		/* 13 bits */
	return len;
}

struct aac_data : public Codec
{
	struct io_stream *stream;
	char rbuf[BUFFER_SIZE];
	int rbuf_len;
	int rbuf_pos;

	int channels;
	int sample_rate;

	char *overflow_buf;
	int overflow_buf_len;

	NeAACDecHandle decoder;	/* typedef void * */

	bool ok; /* was this stream successfully opened? */

	int bitrate;
	int avg_bitrate;
	int duration;


	aac_data(io_stream *stream_, const char *fname)
	: stream(NULL)
	, rbuf_len(0)
	, rbuf_pos(0)
	, channels(0)
	, sample_rate(0)
	, overflow_buf(NULL)
	, overflow_buf_len(0)
	, decoder(NULL)
	, bitrate(0)
	, avg_bitrate(0)
	, duration(0)
	, ok(false)
	{
		decoder = NeAACDecOpen();

		/* set decoder config */
		auto *neaac_cfg = NeAACDecGetCurrentConfiguration(decoder);
		neaac_cfg->outputFormat = FAAD_FMT_16BIT;	/* force 16 bit audio */
		neaac_cfg->downMatrix = 1;			/* 5.1 -> stereo */
		neaac_cfg->dontUpSampleImplicitSBR = 0;		/* upsample, please! */
		NeAACDecSetConfiguration(decoder, neaac_cfg);

		if (stream_)
			stream = stream_;
		else {
			stream = io_open (fname);
			if (!io_ok(stream)) {
				error.fatal("Can't open AAC file: %s", io_strerror(stream));
				return;
			}
		}

		/* find a frame */
		if (buffer_fill_frame() <= 0) {
			error.fatal("Not a valid (or unsupported) AAC file");
			return;
		}

		/* in case of a bug, make sure there is at least some data
		* in the buffer for NeAACDecInit() to work with.
		*/
		if (buffer_fill_min(256) <= 0) {
			error.fatal("AAC file/stream too short");
			return;
		}

		/* init decoder, returns the length of the header (if any) */
		unsigned char ch = (unsigned char)channels;
		unsigned long sr = sample_rate;
		int n = NeAACDecInit (decoder, (unsigned char*) buffer_data(), buffer_length(), &sr, &ch);
		channels = ch;
		sample_rate = (int)sr;
		if (n < 0) {
			error.fatal("libfaad can't open this stream");
			return;
		}

		logit ("sample rate %dHz, channels %d", sample_rate, channels);
		if (!sample_rate || !channels) {
			error.fatal("Invalid AAC sound parameters");
			return;
		}

		/* skip the header */
		logit ("skipping header (%d bytes)", n);
		buffer_consume(n);

		/*NeAACDecInitDRM(decoder, sample_rate, channels);*/

		ok = true;
	}

	~aac_data()
	{
		if (decoder) NeAACDecClose (decoder);
		if (stream)  io_close (stream);
	}

	int   buffer_length() const { return rbuf_len - rbuf_pos; }
	void* buffer_data () { return rbuf + rbuf_pos; }

	int buffer_fill ()
	{
		if (rbuf_pos > 0) {
			rbuf_len = buffer_length ();
			memmove (rbuf, rbuf + rbuf_pos, rbuf_len);
			rbuf_pos = 0;
		}

		if (rbuf_len == BUFFER_SIZE) return 1;

		ssize_t n = io_read (stream, rbuf + rbuf_len, BUFFER_SIZE - rbuf_len);
		if (n == -1) return -1;
		if (n ==  0) return 0;

		rbuf_len += n;
		return 1;
	}
	void buffer_flush () { rbuf_len = rbuf_pos = 0; }
	void buffer_consume (int n)
	{
		assert (n <= buffer_length());
		rbuf_pos += n;
	}

	int buffer_fill_min (int len)
	{
		assert (len < BUFFER_SIZE);
		while (buffer_length() < len) {
			int rc = buffer_fill();
			if (rc <= 0) return rc;
		}
		return 1;
	}

	/* scans forward to the next aac frame and makes sure
	* the entire frame is in the buffer.
	*/
	int buffer_fill_frame()
	{
		unsigned char *datap;
		int rc, n, len;
		int max = 32768;

		while (1) {
			/* need at least 6 bytes of data */
			rc = buffer_fill_min(6);
			if (rc <= 0) break;

			len = buffer_length();
			datap = (unsigned char*) buffer_data();

			/* scan for a frame */
			for (n = 0; n < len - 5; n++) {
				/* give up after 32KB */
				if (max-- == 0) {
					logit ("no frame found!");
					/* FIXME: set errno? */
					return -1;
				}

				/* see if there's a frame at this location */
				rc = parse_frame(datap + n);
				if (rc == 0) continue;

				/* found a frame, consume all data up to the frame */
				buffer_consume (n);

				/* rc == frame length */
				rc = buffer_fill_min (rc);
				if (rc <= 0) goto end;

				return 1;
			}

			/* consume what we used */
			buffer_consume (n);
		}
	end:
		return rc;
	}

	/* This should be called with a unique decoder instance as the seeking
	* it does triggers an FAAD bug which results in distorted audio due to
	* retained state being corrupted.  (One suspects NeAACDecPostSeekReset()
	* should resolve the problem but experimentation suggests not and no
	* documentation exists describing its use.) */
	int aac_count_time ()
	{
		NeAACDecFrameInfo frame_info;
		int samples = 0, bytes = 0, frames = 0;
		off_t file_size;
		int16_t *sample_buf;

		file_size = io_file_size (stream);
		if (file_size == -1) return -1;

		if (io_seek(stream, file_size / 2, SEEK_SET) == -1) return -1;
		buffer_flush();

		/* Guess track length by decoding the middle 50 frames which have
		* more than 25% of samples having absolute values greater than 16. */
		while (frames < 50) {
			if (buffer_fill_frame() <= 0) break;

			sample_buf = (int16_t*) NeAACDecDecode (decoder, &frame_info,
						(unsigned char*) buffer_data(), buffer_length());

			if (frame_info.error == 0 && frame_info.samples > 0) {
				unsigned int ix, zeroes = 0;

				for (ix = 0; ix < frame_info.samples; ix += 1) {
					if (RANGE(-16, sample_buf[ix], 16))
						zeroes += 1;
				}

				if (zeroes * 4 < frame_info.samples) {
					samples += frame_info.samples;
					bytes += frame_info.bytesconsumed;
					frames += 1;
				}
			}

			if (frame_info.bytesconsumed == 0) break;

			buffer_consume (frame_info.bytesconsumed);
		}

		if (frames == 0) return -1;

		samples /= frames;
		samples /= channels;
		bytes /= frames;

		return ((file_size / bytes) * samples) / sample_rate;
	}

	/* returns -1 on fatal errors
	* returns -2 on non-fatal errors
	* 0 on eof
	* number of bytes put in 'buffer' on success */
	int decode_one_frame (void *buffer, int count)
	{
		unsigned char *aac_data;
		unsigned int aac_data_size;
		NeAACDecFrameInfo frame_info;
		char *sample_buf;
		int bytes, rc;

		rc = buffer_fill_frame ();
		if (rc <= 0) return rc;

		aac_data = (unsigned char*) buffer_data ();
		aac_data_size = buffer_length ();

		/* aac data -> raw pcm */
		sample_buf = (char*)NeAACDecDecode (decoder, &frame_info,
					aac_data, aac_data_size);

		buffer_consume (frame_info.bytesconsumed);

		if (!sample_buf || frame_info.bytesconsumed <= 0) {
			error.fatal("%s", NeAACDecGetErrorMessage (frame_info.error));
			return -1;
		}

		if (frame_info.error != 0) {
			error.warn("%s", NeAACDecGetErrorMessage(frame_info.error));
			return -2;
		}

		if (frame_info.samples <= 0)
			return -2;

		if (frame_info.channels != (unsigned char)channels ||
		frame_info.samplerate != (unsigned long)sample_rate) {
			error.warn("Invalid channel or sample_rate count");
			return -2;
		}

		/* 16-bit samples */
		bytes = frame_info.samples * 2;

		if (bytes > count) {
			/* decoded too much, keep overflow */
			overflow_buf = sample_buf + count;
			overflow_buf_len = bytes - count;
			memcpy (buffer, sample_buf, count);
			return count;
		}

		memcpy (buffer, sample_buf, bytes);

		bitrate = frame_info.bytesconsumed * 8 / (bytes / 2.0 /
				channels / sample_rate) / 1000;

		return bytes;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		error.clear();

		sound_params.channels = channels;
		sound_params.rate = sample_rate;
		sound_params.fmt = SFMT_S16 | SFMT_NE;

		/* use overflow from previous call (if any) */
		if (overflow_buf_len) {
			int len = MIN(overflow_buf_len, buf_len);

			memcpy (buf, overflow_buf, len);
			overflow_buf += len;
			overflow_buf_len -= len;
			return len;
		}

		int rc;
		do {
			rc = decode_one_frame (buf, buf_len);
		} while (rc == -2);

		return MAX(rc, 0);
	}

	int get_bitrate () const override
	{
		return bitrate;
	}

	int get_avg_bitrate () const override
	{
		return avg_bitrate / 1000;
	}

	int get_duration () const override
	{
		return duration;
	}
};

struct aac_decoder : public Decoder
{
	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "aac");
	}

	bool matches_mime (const str &mime) const override
	{
		return !strcasecmp (mime.c_str(), "audio/aac")
			|| !strncasecmp (mime.c_str(), "audio/aac;", 10)
			|| !strcasecmp  (mime.c_str(), "audio/aacp")
			|| !strncasecmp (mime.c_str(), "audio/aacp;", 11);
	}

	Codec* open(const str &file) override
	{
		aac_data *data = new aac_data(NULL, file.c_str());

		if (data->ok) {
			int duration = data->aac_count_time();
			int avg_bitrate = -1;
			off_t file_size = io_file_size (data->stream);

			if (duration > 0 && file_size != -1)
				avg_bitrate = file_size / duration * 8;
			delete data;
			data = new aac_data(NULL, file.c_str());
			data->duration = duration;
			data->avg_bitrate = avg_bitrate;
		}

		return data;
	}

	Codec* open(io_stream &stream) override
	{
		return new aac_data(&stream, NULL);
	}

	str get_tag (struct id3_tag *tag, const char *what)
	{
		struct id3_frame *frame;
		union id3_field *field;
		const id3_ucs4_t *ucs4;
		char *comm = NULL;

		frame = id3_tag_findframe (tag, what, 0);
		if (frame && (field = &frame->fields[1])) {
			ucs4 = id3_field_getstrings (field, 0);
			if (ucs4)
				comm = (char *)id3_ucs4_utf8duplicate (ucs4);
		}
		
		str ret;
		if (comm) ret = comm;
		free(comm);
		return ret;
	}

	/* Fill info structure with data from aac comments */
	void read_tags(const str &file_name, file_tags &info) override
	{
		struct id3_file *id3file = id3_file_open (file_name.c_str(), ID3_FILE_MODE_READONLY);
		if (!id3file) return;
		struct id3_tag *tag = id3_file_tag (id3file);
		if (tag) {
			info.artist = get_tag (tag, ID3_FRAME_ARTIST);
			info.title  = get_tag (tag, ID3_FRAME_TITLE);
			info.album  = get_tag (tag, ID3_FRAME_ALBUM);
			str track   = get_tag (tag, ID3_FRAME_TRACK);

			if (!track.empty()) {
				char *end;
				info.track = strtol (track.c_str(), &end, 10);
				if (end == track.c_str())
					info.track = -1;
			}
		}
		id3_file_close (id3file);

		aac_data data(NULL, file_name.c_str());
		if (data.ok)
			info.time = data.aac_count_time();
		else
			logit ("%s", data.error.desc.c_str());
	}
};

Decoder *aac_plugin ()
{
	return new aac_decoder;
}

