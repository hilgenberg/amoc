/*
 * MOC - music on console
 * Copyright (C) 2002 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <limits.h>
#include <inttypes.h>
#ifndef HAVE_TREMOR
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>
#else
#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>
#endif

#include "decoder.h"
#include "io.h"
#include "../audio.h"

/* Tremor defines time as 64-bit integer milliseconds. */
#ifndef HAVE_TREMOR
static constexpr int64_t time_scaler = 1;
#else
static constexpr int64_t time_scaler = 1000;
#endif

static size_t read_cb (void *ptr, size_t size, size_t nmemb, void *datasource)
{
	ssize_t res = io_read ((io_stream*) datasource, ptr, size * nmemb);

	/* libvorbisfile expects the read callback to return >= 0 with errno
	* set to non zero on error. */
	if (res < 0) {
		logit ("Read error");
		if (errno == 0) errno = 0xffff;
		res = 0;
	}
	else
		res /= size;

	return res;
}

static int seek_cb (void *datasource, ogg_int64_t offset, int whence)
{
	debug ("Seek request to %" PRId64 " (%s)", offset,
			whence == SEEK_SET ? "SEEK_SET"
			: (whence == SEEK_CUR ? "SEEK_CUR" : "SEEK_END"));
	return io_seek ((io_stream*) datasource, offset, whence) == -1 ? -1 : 0;
}

static int close_cb (void *unused)
{
	return 0;
}

static long tell_cb (void *datasource)
{
	return (long)io_tell ((io_stream*) datasource);
}

static void get_comment_tags (OggVorbis_File *vf, struct file_tags *info)
{
	vorbis_comment *comments = ov_comment (vf, -1);
	for (int i = 0; i < comments->comments; i++) {
		if (!strncasecmp(comments->user_comments[i], "title=",
				 strlen ("title=")))
			info->title = (comments->user_comments[i]
					+ strlen ("title="));
		else if (!strncasecmp(comments->user_comments[i],
					"artist=", strlen ("artist=")))
			info->artist = (comments->user_comments[i]
					+ strlen ("artist="));
		else if (!strncasecmp(comments->user_comments[i],
					"album=", strlen ("album=")))
			info->album = (comments->user_comments[i]
					+ strlen ("album="));
		else if (!strncasecmp(comments->user_comments[i],
					"tracknumber=",
					strlen ("tracknumber=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("tracknumber="));
		else if (!strncasecmp(comments->user_comments[i],
					"track=", strlen ("track=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("track="));
	}
}

/* Return a description of an ov_*() error. */
static const char *vorbis_strerror (const int code)
{
	switch (code) {
		case OV_EREAD: return "read error";
		case OV_ENOTVORBIS: return "not a vorbis file";
		case OV_EVERSION: return "vorbis version mismatch";
		case OV_EBADHEADER: return "invalid Vorbis bitstream header";
		case OV_EFAULT: return "internal (vorbis) logic fault";
		default: return "unknown error";
	}
}

struct vorbis_data : public Codec
{
	struct io_stream *stream;
	OggVorbis_File vf;
	int last_section;
	int bitrate;
	int avg_bitrate;
	int duration;
	int ok; /* was this stream successfully opened? */
	int tags_change; /* the tags were changed from the last call of
	                    ogg_current_tags() */
	struct file_tags *tags;


	vorbis_data(struct io_stream *stream)
	: stream(stream)
	, ok(0)
	, tags_change(0)
	, tags(new file_tags)
	{
		ov_callbacks callbacks = {
			read_cb,
			seek_cb,
			close_cb,
			tell_cb
		};

		int res = ov_open_callbacks (stream, &vf, NULL, 0, callbacks);
		if (res < 0) {
			const char *vorbis_err = vorbis_strerror (res);
			error.fatal("%s", vorbis_err);
			debug ("ov_open error: %s", vorbis_err);
			return;
		}
		int64_t duration;

		last_section = -1;
		avg_bitrate = ov_bitrate (&vf, -1) / 1000;
		bitrate = avg_bitrate;
		duration = -1;
		duration = ov_time_total (&vf, -1);
		if (duration >= 0) duration /= time_scaler;
		ok = 1;
		get_comment_tags(&vf, tags);
	}

	~vorbis_data()
	{
		if (ok) ov_clear (&vf);
		delete stream;
		delete tags;
	}

	int seek (int sec) override
	{
		return ov_time_seek (&vf, sec * time_scaler) ? -1 : sec;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		int ret;
		int current_section;
		int bitrate;
		vorbis_info *info;

		error.clear();

		while (1) {
			#ifndef HAVE_TREMOR
			ret = ov_read(&vf, buf, buf_len,
				(SFMT_NE == SFMT_LE ? 0 : 1),
				2, 1, &current_section);
			#else
			ret = ov_read(&vf, buf, buf_len, &current_section);
			#endif
			if (ret == 0) return 0;
			if (ret < 0) {
				error.warn("Error in the stream!");
				continue;
			}

			if (current_section != last_section) {
				logit ("section change or first section");

				last_section = current_section;
				tags_change = 1;
				delete tags;
				tags = new file_tags;
				get_comment_tags (&vf, tags);
			}

			info = ov_info (&vf, -1);
			assert (info != NULL);
			sound_params.channels = info->channels;
			sound_params.rate = info->rate;
			sound_params.fmt = SFMT_S16 | SFMT_NE;

			/* Update the bitrate information */
			bitrate = ov_bitrate_instant (&vf);
			if (bitrate > 0) bitrate /= 1000;

			break;
		}

		return ret;
	}

	bool current_tags (file_tags &t) override
	{
		if (!tags_change) return false;
		if (tags) t = *tags;
		tags_change = 0;
		return true;
	}

	int get_bitrate () const override { return bitrate; }
	int get_avg_bitrate () const override { return avg_bitrate; }
	int get_duration () const override { return duration; }
};

struct vorbis_decoder : public Decoder
{
	void read_tags (const str &file_name, file_tags &info) override
	{
		FILE *file = fopen (file_name.c_str(), "r");
		if (!file) {
			log_errno ("Can't open an OGG file", errno);
			return;
		}

		OggVorbis_File vf;
		int err_code = ov_open(file, &vf, NULL, 0);
		/* ov_test() is faster than ov_open(), but we can't read file time
		* with it. */
		//err_code = ov_test(file, &vf, NULL, 0);

		if (err_code < 0) {
			logit ("Can't open %s: %s", file_name, vorbis_strerror (err_code));
			fclose (file);
			return;
		}

		get_comment_tags (&vf, &info);

		int64_t vorbis_time = ov_time_total (&vf, -1);
		if (vorbis_time >= 0) info.time = vorbis_time / time_scaler;

		ov_clear (&vf);
	}

	Codec* open(io_stream &stream) override
	{
		return new vorbis_data(&stream);
	}

	#if INT64_MAX > LONG_MAX
	Codec* open(const str &file) override
	{
		auto *stream = io_open (file, 1);
		if (!io_ok(stream)) {
			delete stream;
			return NULL;
		}

		/* This a restriction placed on us by the vorbisfile API. */
		if (io_file_size (stream) > LONG_MAX) {
			delete stream;
			return NULL;
		}

		return open(*stream);
	}
	#endif

	bool can_decode (io_stream &stream) override
	{
		char buf[35];
		return io_peek(&stream, buf, 35) == 35 && !memcmp(buf, "OggS", 4)
				&& !memcmp (buf + 28, "\01vorbis", 7);
	}

	bool matches_ext (const char *ext) const override
	{
		return !strcasecmp (ext, "ogg")
			|| !strcasecmp (ext, "oga");
	}

	bool matches_mime (const str &mime)
	{
		return !strcasecmp (mime.c_str(), "application/ogg")
			|| !strncasecmp (mime.c_str(), "application/ogg;", 16)
			|| !strcasecmp (mime.c_str(), "application/x-ogg")
			|| !strncasecmp (mime.c_str(), "application/x-ogg;", 18);
	}
};

Decoder *vorbis_plugin ()
{
	return new vorbis_decoder;
}
