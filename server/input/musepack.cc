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

/* FIXME: mpc_decoder_decode() can give fixed point values, do we have to
 * handle this case? */

#include <inttypes.h>
#include <mpc/mpcdec.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include "decoder.h"
#include "io.h"
#include "../audio.h"

static mpc_int32_t read_cb (mpc_reader *t, void *buf, mpc_int32_t size);
static mpc_bool_t seek_cb (mpc_reader *t, mpc_int32_t offset);
static mpc_int32_t tell_cb (mpc_reader *t);
static mpc_int32_t get_size_cb (mpc_reader *t);
static mpc_bool_t canseek_cb (mpc_reader *t);

struct musepack_data : public Codec
{
	struct io_stream *stream;
	mpc_demux *demux;
	mpc_reader reader;
	mutable mpc_streaminfo info;
	int avg_bitrate;
	int bitrate;
	int ok; /* was this stream successfully opened? */
	float *remain_buf;
	size_t remain_buf_len; /* in samples (sizeof(float)) */

	void init ()
	{
		reader.read = read_cb;
		reader.seek = seek_cb;
		reader.tell = tell_cb;
		reader.get_size = get_size_cb;
		reader.canseek = canseek_cb;
		reader.data = this;

		demux = mpc_demux_init (&reader);
		if (!demux) {
			error.fatal("Not a valid MPC file.");
			return;
		}

		mpc_demux_get_info (demux, &info);

		avg_bitrate = (int) (info.average_bitrate / 1000);
		debug ("Avg bitrate: %d", avg_bitrate);

		remain_buf = NULL;
		remain_buf_len = 0;
		bitrate = 0;
		ok = 1;
	}

	~musepack_data()
	{
		if (ok) {
			mpc_demux_exit (demux);
			free (remain_buf);
		}
		delete stream; stream = NULL;
	}

	int seek (int sec) override
	{
		if (mpc_demux_seek_second (demux, sec) != MPC_STATUS_OK) return -1;
		
		if (remain_buf) {
			free (remain_buf);
			remain_buf = NULL;
			remain_buf_len = 0;
		}

		return sec;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		int decoded;
		int bytes_from_decoder;
		mpc_frame_info frame;
		mpc_status err;
		float decode_buf[MPC_DECODER_BUFFER_LENGTH];
		if (remain_buf) {
			size_t to_copy = MIN((unsigned int)buf_len,
					remain_buf_len * sizeof(float));

			debug ("Copying %zu bytes from the remain buf", to_copy);

			memcpy (buf, remain_buf, to_copy);
			if (to_copy / sizeof(float) < remain_buf_len) {
				memmove (remain_buf, remain_buf + to_copy,
						remain_buf_len * sizeof(float)
						- to_copy);
				remain_buf_len -= to_copy / sizeof(float);
			}
			else {
				debug ("Remain buf is now empty");
				free (remain_buf);
				remain_buf = NULL;
				remain_buf_len = 0;
			}

			return to_copy;
		}

		do {
			frame.buffer = decode_buf;
			err = mpc_demux_decode (demux, &frame);

			if (err == MPC_STATUS_OK && frame.bits == -1) {
				debug ("EOF");
				return 0;
			}

			if (err == MPC_STATUS_OK)
				continue;

			if (frame.bits == -1) {
				error.fatal("Error in the stream!");
				return 0;
			}

			error.warn("Broken frame.");
		} while (err != MPC_STATUS_OK || frame.samples == 0);

		mpc_demux_get_info (demux, &info);
		bytes_from_decoder = frame.samples * sizeof(MPC_SAMPLE_FORMAT) * info.channels;
		bitrate = info.bitrate;

		error.clear();
		sound_params.channels = info.channels;
		sound_params.rate = info.sample_freq;
		sound_params.fmt = SFMT_FLOAT;

		if (bytes_from_decoder >= buf_len) {
			size_t to_copy = MIN (buf_len, bytes_from_decoder);

			debug ("Copying %zu bytes", to_copy);

			memcpy (buf, decode_buf, to_copy);
			remain_buf_len = (bytes_from_decoder - to_copy)
				/ sizeof(float);
			remain_buf = (float *)xmalloc (remain_buf_len *
					sizeof(float));
			memcpy (remain_buf, decode_buf + to_copy,
					remain_buf_len * sizeof(float));
			decoded = to_copy;
		}
		else {
			debug ("Copying whole decoded sound (%d bytes)", bytes_from_decoder);
			memcpy (buf, decode_buf, bytes_from_decoder);
			decoded = bytes_from_decoder;
		}

		return decoded;
	}

	int get_bitrate () const override
	{
		return bitrate;
	}

	int get_avg_bitrate () const override
	{
		return avg_bitrate;
	}

	int get_duration () const override
	{
		return mpc_streaminfo_get_length (&info);
	}
};

static mpc_int32_t read_cb (mpc_reader *t, void *buf, mpc_int32_t size)
{
	struct musepack_data *data = (struct musepack_data*) t->data;
	ssize_t res;

	res = io_read (data->stream, buf, size);
	if (res < 0) {
		logit ("Read error");
		res = 0;
	}

	return res;
}

static mpc_bool_t seek_cb (mpc_reader *t, mpc_int32_t offset)
{
	struct musepack_data *data = (struct musepack_data*) t->data;
	return io_seek(data->stream, offset, SEEK_SET) >= 0 ? 1 : 0;
}

static mpc_int32_t tell_cb (mpc_reader *t)
{
	struct musepack_data *data = (struct musepack_data*) t->data;
	return (mpc_int32_t)io_tell (data->stream);
}

static mpc_int32_t get_size_cb (mpc_reader *t)
{
	struct musepack_data *data = (struct musepack_data*) t->data;
	return (mpc_int32_t)io_file_size (data->stream);
}

static mpc_bool_t canseek_cb (mpc_reader *t)
{
	return true;
}

struct musepack_decoder : public Decoder
{
	Codec* open (const str &file) override
	{
		musepack_data *data = new musepack_data;
		data->ok = 0;

		try {
			data->stream = new io_stream(file.c_str());
		}
		catch (std::exception &e)
		{
			data->error.fatal("Can't open file: %s", e.what());
			return data;
		}

		/* This a restriction placed on us by the Musepack API. */
		if (io_file_size (data->stream) > INT32_MAX) {
			data->error.fatal("File too large!");
			return data;
		}

		data->init();
		return data;
	}

	Codec* open(io_stream &stream) override
	{
		musepack_data *data = new musepack_data;
		data->ok = 0;
		data->stream = &stream;
		data->init();
		return data;
	}

	int get_duration(const str &file) override
	{
		musepack_data *data = (musepack_data*)open(file);
		if (!data) return -1;
		int d = (data->error.type == ERROR_OK ? data->get_duration() : -1);
		delete data;
		return d;
	}

	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "mpc");
	}
};

Decoder *muse_plugin ()
{
	return new musepack_decoder;
}
