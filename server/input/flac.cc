/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* The code is based on libxmms-flac written by Josh Coalson. */

#include "decoder.h"
#include "io.h"
#include "../audio.h"
#include "../server.h"
#include <FLAC/all.h>

#define MAX_SUPPORTED_CHANNELS		2
#define SAMPLES_PER_WRITE		512
#define SAMPLE_BUFFER_SIZE ((FLAC__MAX_BLOCK_SIZE + SAMPLES_PER_WRITE) * MAX_SUPPORTED_CHANNELS * (32/8))

/* Convert FLAC big-endian data into PCM little-endian. */
static size_t pack_pcm_signed (FLAC__byte *data,
		const FLAC__int32 * const input[], unsigned int wide_samples,
		unsigned int channels, unsigned int bps)
{
	FLAC__byte * const start = data;
	FLAC__int32 sample;
	const FLAC__int32 *input_;
	unsigned int samples, channel;
	unsigned int bytes_per_sample;
	unsigned int incr;

	if (bps == 24)
		bps = 32; /* we encode to 32-bit words */
	bytes_per_sample = bps / 8;
	incr = bytes_per_sample * channels;

	for (channel = 0; channel < channels; channel++) {
		samples = wide_samples;
		data = start + bytes_per_sample * channel;
		input_ = input[channel];

		while(samples--) {
			sample = *input_++;

			switch(bps) {
				case 8:
					data[0] = sample;
					break;
				case 16:
					data[1] = (FLAC__byte)(sample >> 8);
					data[0] = (FLAC__byte)sample;
					break;
				case 32:
					data[3] = (FLAC__byte)(sample >> 16);
					data[2] = (FLAC__byte)(sample >> 8);
					data[1] = (FLAC__byte)sample;
					data[0] = 0;
					break;
			}

			data += incr;
		}
	}

	debug ("Converted %u bytes", wide_samples * channels * bytes_per_sample);

	return wide_samples * channels * bytes_per_sample;
}

static FLAC__StreamDecoderWriteStatus write_cb (const FLAC__StreamDecoder *, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data);
static void metadata_cb (const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *client_data);
static void error_cb (const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *client_data);
static FLAC__StreamDecoderReadStatus read_cb (const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *client_data);
static FLAC__StreamDecoderSeekStatus seek_cb (const FLAC__StreamDecoder *, FLAC__uint64 absolute_byte_offset, void *client_data);
static FLAC__StreamDecoderTellStatus tell_cb (const FLAC__StreamDecoder *, FLAC__uint64 *absolute_byte_offset, void *client_data);
static FLAC__StreamDecoderLengthStatus length_cb (const FLAC__StreamDecoder *, FLAC__uint64 *stream_length, void *client_data);
static FLAC__bool eof_cb (const FLAC__StreamDecoder *, void *client_data);

struct flac_data : public Codec
{
	FLAC__StreamDecoder *decoder;
	struct io_stream *stream;
	int bitrate;
	int avg_bitrate;
	int abort; /* abort playing (due to an error) */

	unsigned int length;
	FLAC__uint64 total_samples;

	FLAC__byte sample_buffer[SAMPLE_BUFFER_SIZE];
	unsigned int sample_buffer_fill;

	/* sound parameters */
	unsigned int bits_per_sample;
	unsigned int sample_rate;
	unsigned int channels;

	FLAC__uint64 last_decode_position;

	int ok; /* was this stream successfully opened? */

	flac_data(const char *file, const int buffered)
	{
		decoder = NULL;
		bitrate = -1;
		avg_bitrate = -1;
		abort = 0;
		sample_buffer_fill = 0;
		last_decode_position = 0;
		length = -1;
		ok = 0;

		stream = io_open (file, buffered);
		if (!io_ok(stream)) {
			error.fatal("Can't load file: %s", io_strerror(stream));
			return;
		}

		if (!(decoder = FLAC__stream_decoder_new())) {
			error.fatal("FLAC__stream_decoder_new() failed");
			return;
		}

		FLAC__stream_decoder_set_md5_checking (decoder, false);

		FLAC__stream_decoder_set_metadata_ignore_all (decoder);
		FLAC__stream_decoder_set_metadata_respond (decoder, FLAC__METADATA_TYPE_STREAMINFO);

		if (FLAC__stream_decoder_init_stream(decoder, read_cb, seek_cb, tell_cb, length_cb, eof_cb, write_cb, metadata_cb, error_cb, this)
				!= FLAC__STREAM_DECODER_INIT_STATUS_OK) {
			error.fatal("FLAC__stream_decoder_init() failed");
			return;
		}

		if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder)) {
			error.fatal("FLAC__stream_decoder_process_until_end_of_metadata() failed.");
			return;
		}

		ok = 1;

		if (length > 0) {
			off_t data_size = io_file_size (stream);
			if (data_size > 0) {
				FLAC__uint64 pos;

				if (FLAC__stream_decoder_get_decode_position (decoder, &pos))
					data_size -= pos;
				avg_bitrate = data_size * 8 / length;
			}
		}
	}

	~flac_data()
	{
		if (decoder) {
			FLAC__stream_decoder_finish (decoder);
			FLAC__stream_decoder_delete (decoder);
		}
		io_close (stream);
	}

	int seek (int sec) override
	{
		FLAC__uint64 target_sample;

		if ((unsigned int)sec > length) return -1;

		target_sample = (FLAC__uint64)(((double)sec / (double)length) *
					(double)total_samples);


		if (FLAC__stream_decoder_seek_absolute(decoder, target_sample))
			return sec;

		logit ("FLAC__stream_decoder_seek_absolute() failed.");

		return -1;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		unsigned int to_copy;
		int bytes_per_sample;
		FLAC__uint64 decode_position;

		bytes_per_sample = bits_per_sample / 8;

		switch (bytes_per_sample) {
			case 1: sound_params.fmt = SFMT_S8; break;
			case 2: sound_params.fmt = SFMT_S16 | SFMT_LE; break;
			case 3: sound_params.fmt = SFMT_S32 | SFMT_LE; break;
		}

		sound_params.rate = sample_rate;
		sound_params.channels = channels;

		error.clear();

		if (!sample_buffer_fill) {
			debug ("decoding...");

			if (FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
				logit ("EOF");
				return 0;
			}

			if (!FLAC__stream_decoder_process_single(decoder)) {
				error.fatal("Read error processing frame.");
				return 0;
			}

			/* Count the bitrate */
			if(!FLAC__stream_decoder_get_decode_position(decoder, &decode_position))
				decode_position = 0;
			if (decode_position > last_decode_position) {
				int bytes_per_sec = bytes_per_sample * sample_rate
					* channels;

				bitrate = (decode_position
					- last_decode_position) * 8.0
					/ (sample_buffer_fill
							/ (float)bytes_per_sec)
					/ 1000;
			}

			last_decode_position = decode_position;
		}
		else
			debug ("Some date remain in the buffer.");

		debug ("Decoded %d bytes", sample_buffer_fill);

		to_copy = MIN((unsigned int)buf_len, sample_buffer_fill);
		memcpy (buf, sample_buffer, to_copy);
		memmove (sample_buffer, sample_buffer + to_copy,
				sample_buffer_fill - to_copy);
		sample_buffer_fill -= to_copy;

		return to_copy;
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
		return ok ? length : -1;
	}
};

static void fill_tag (FLAC__StreamMetadata_VorbisComment_Entry *comm,
		struct file_tags *tags)
{
	char *name, *value;
	FLAC__byte *eq;
	int value_length;

	eq = (FLAC__byte*)memchr (comm->entry, '=', comm->length);
	if (!eq)
		return;

	name = (char *)xmalloc (sizeof(char) * (eq - comm->entry + 1));
	strncpy (name, (char *)comm->entry, eq - comm->entry);
	name[eq - comm->entry] = 0;
	value_length = comm->length - (eq - comm->entry + 1);

	if (value_length == 0) {
		free (name);
		return;
	}

	value = (char *)xmalloc (sizeof(char) * (value_length + 1));
	strncpy (value, (char *)(eq + 1), value_length);
	value[value_length] = 0;

	if (!strcasecmp(name, "title"))
		tags->title = value;
	else if (!strcasecmp(name, "artist"))
		tags->artist = value;
	else if (!strcasecmp(name, "album"))
		tags->album = value;
	else if (!strcasecmp(name, "tracknumber")
			|| !strcasecmp(name, "track")) {
		tags->track = atoi (value);
		free (value);
	}
	else
		free (value);

	free (name);
}

static void get_vorbiscomments (const char *filename, struct file_tags *tags)
{
	FLAC__Metadata_SimpleIterator *iterator
		= FLAC__metadata_simple_iterator_new();
	FLAC__bool got_vorbis_comments = false;

	debug ("Reading comments for %s", filename);

	if (!iterator) {
		logit ("FLAC__metadata_simple_iterator_new() failed.");
		return;
	}

	if (!FLAC__metadata_simple_iterator_init(iterator, filename, true,
				true)) {
		logit ("FLAC__metadata_simple_iterator_init failed.");
		FLAC__metadata_simple_iterator_delete(iterator);
		return;
	}

	do {
		if (FLAC__metadata_simple_iterator_get_block_type(iterator)
				== FLAC__METADATA_TYPE_VORBIS_COMMENT) {
			FLAC__StreamMetadata *block;

			block = FLAC__metadata_simple_iterator_get_block (
					iterator);
			if (block) {
				unsigned int i;
				const FLAC__StreamMetadata_VorbisComment *vc
					= &block->data.vorbis_comment;

				for (i = 0; i < vc->num_comments; i++)
					fill_tag (&vc->comments[i], tags);

				FLAC__metadata_object_delete (block);
				got_vorbis_comments = true;
			}
		}
	} while (!got_vorbis_comments
			&& FLAC__metadata_simple_iterator_next(iterator));

	FLAC__metadata_simple_iterator_delete(iterator);
}

struct flac_decoder : public Decoder
{
	Codec* open (const str &file) override
	{
		return new flac_data(file.c_str(), 1);
	}


	void read_tags(const str &file_name, file_tags &info) override
	{
		struct flac_data *data = new flac_data(file_name.c_str(), 0);
		if (data->ok) info.time = data->length;
		delete data;

		get_vorbiscomments (file_name.c_str(), &info);
	}

	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "flac") || !strcasecmp (ext, "fla");
	}

	bool matches_mime (const str &mime) const override
	{
		return !strcasecmp (mime.c_str(), "audio/flac") ||
		!strncasecmp (mime.c_str(), "audio/flac;", 11) ||
		!strcasecmp (mime.c_str(), "audio/x-flac") ||
		!strncasecmp (mime.c_str(), "audio/x-flac;", 13);
	}
};

static FLAC__StreamDecoderWriteStatus write_cb (const FLAC__StreamDecoder *, const FLAC__Frame *frame,
		const FLAC__int32 * const buffer[], void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	const unsigned int wide_samples = frame->header.blocksize;

	if (data->abort)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	data->sample_buffer_fill = pack_pcm_signed (
			data->sample_buffer, buffer, wide_samples,
			data->channels, data->bits_per_sample);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_cb (const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		debug ("Got metadata info");

		data->total_samples = metadata->data.stream_info.total_samples;
		data->bits_per_sample = metadata->data.stream_info.bits_per_sample;
		data->channels = metadata->data.stream_info.channels;
		data->sample_rate = metadata->data.stream_info.sample_rate;
		if (data->total_samples > 0)
			data->length = data->total_samples / data->sample_rate;
	}
}

static void error_cb (const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
		debug ("Aborting due to error");
		data->abort = 1;
	}
	else
		data->error.fatal("FLAC: lost sync");
}

static FLAC__StreamDecoderReadStatus read_cb (const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	ssize_t res = io_read (data->stream, buffer, *bytes);

	if (res > 0) {
		*bytes = res;
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
	}

	if (res == 0) {
		*bytes = 0;
		/* not sure why this works, but if it ain't broke... */
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	error ("read error: %s", io_strerror(data->stream));

	return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

static FLAC__StreamDecoderSeekStatus seek_cb (const FLAC__StreamDecoder *, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	return io_seek(data->stream, absolute_byte_offset, SEEK_SET) >= 0
		? FLAC__STREAM_DECODER_SEEK_STATUS_OK
		: FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
}

static FLAC__StreamDecoderTellStatus tell_cb (const FLAC__StreamDecoder *, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	*absolute_byte_offset = io_tell (data->stream);
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus length_cb (const FLAC__StreamDecoder *, FLAC__uint64 *stream_length, void *client_data)
{
	off_t file_size;
	struct flac_data *data = (struct flac_data *)client_data;

	file_size = io_file_size (data->stream);
	if (file_size == -1)
		return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;

	*stream_length = file_size;

	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool eof_cb (const FLAC__StreamDecoder *, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	return io_eof (data->stream);
}

Decoder *flac_plugin ()
{
	return new flac_decoder;
}
