/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Based on FFplay Copyright (c) 2003 Fabrice Bellard
 *
 */

/*
 *		"The main problem is that external projects who want to
 *		 support both FFmpeg and LibAV are just fucked, and this
 *		 only because LibAV doesn't care a second about their users."
 *
 *		-- http://blog.pkh.me/p/13-the-ffmpeg-libav-situation.html
 */

#include "../audio.h"
#include "decoder.h"
#include <pthread.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
}

#undef STRERROR_FN
#define STRERROR_FN ffmpeg_strerror

/* Set SEEK_IN_DECODER to 1 if you'd prefer seeking to be delay until
 * the next time ffmpeg_decode() is called.  This will provide seeking
 * in formats for which FFmpeg falsely reports seek errors, but could
 * result erroneous current time values. */
#define SEEK_IN_DECODER 0

struct extn_list {
	const char *extn;
	const char *format;
};
static std::set<std::string> supported_extns;

/* FFmpeg-provided error code to description function wrapper. */
static inline char *ffmpeg_strerror (int errnum)
{
	char *result;

	result = (char*) xmalloc (256);
	av_strerror (errnum, result, 256);
	result[255] = 0;

	return result;
}

/* Find the first audio stream and return its index, or nb_streams if
 * none found. */
static unsigned int find_first_audio (AVFormatContext *ic)
{
	unsigned int result;

	assert (ic);

	for (result = 0; result < ic->nb_streams; result += 1) {
		if (ic->streams[result]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
			break;
	}

	return result;
}

static void load_audio_extns ()
{
	int ix;

	/* When adding an entry to this list, tests need to be performed to
	 * determine whether or not FFmpeg/LibAV handles durations and seeking
	 * correctly.  If not, then the appropriate additions should be made
	 * in is_timing_broken() and is_seek_broken(). */
	const struct extn_list audio_extns[] = {
		{"aac", "aac"},
		{"ac3", "ac3"},
		{"ape", "ape"},
		{"au", "au"},
		{"ay", "libgme"},
		{"dff", "dsf"},
		{"dsf", "dsf"},
		{"dts", "dts"},
		{"eac3", "eac3"},
		{"fla", "flac"},
		{"flac", "flac"},
		{"gbs", "libgme"},
		{"gym", "libgme"},
		{"hes", "libgme"},
		{"kss", "libgme"},
		{"mka", "matroska"},
		{"mp2", "mpeg"},
		{"mp3", "mp3"},
		{"mpc", "mpc"},
		{"mpc8", "mpc8"},
		{"m4a", "m4a"},
		{"nsf", "libgme"},
		{"nsfe", "libgme"},
		{"ra", "rm"},
		{"sap", "libgme"},
		{"spc", "libgme"},
		{"tta", "tta"},
		{"vgm", "libgme"},
		{"vgz", "libgme"},
		{"vqf", "vqf"},
		{"wav", "wav"},
		{"w64", "w64"},
		{"wma", "asf"},
		{"wv", "wv"},
		{NULL, NULL}
	};

	for (ix = 0; audio_extns[ix].extn; ix += 1) {
		if (av_find_input_format (audio_extns[ix].format))
			supported_extns.insert(audio_extns[ix].extn);
	}

	if (av_find_input_format ("ogg")) {
		supported_extns.insert("ogg");
		if (avcodec_find_decoder (AV_CODEC_ID_VORBIS))
			supported_extns.insert("oga");
		if (avcodec_find_decoder (AV_CODEC_ID_OPUS))
			supported_extns.insert("opus");
		if (avcodec_find_decoder (AV_CODEC_ID_THEORA))
			supported_extns.insert("ogv");
	}

	/* In theory, FFmpeg supports Speex if built with libspeex enabled.
	 * In practice, it breaks badly. */
#if 0
	if (avcodec_find_decoder (AV_CODEC_ID_SPEEX))
		supported_extns.insert("spx");
#endif
}

static void load_video_extns ()
{
	int ix;
	const struct extn_list video_extns[] = {
		{"avi", "avi"},
		{"flv", "flv"},
		{"mkv", "matroska"},
		{"mp4", "mp4"},
		{"rec", "mpegts"},
		{"vob", "mpeg"},
		{"webm", "matroska"},
		{NULL, NULL}
	};

	for (ix = 0; video_extns[ix].extn; ix += 1) {
		if (av_find_input_format (video_extns[ix].format))
			supported_extns.insert(video_extns[ix].extn);
	}
}

/* Handle FFmpeg's locking requirements. */
static int locking_cb (void **mutex, enum AVLockOp op)
{
	int result;

	switch (op) {
	case AV_LOCK_CREATE:
		*mutex = xmalloc (sizeof (pthread_mutex_t));
		result = pthread_mutex_init ((pthread_mutex_t*) *mutex, NULL);
		break;
	case AV_LOCK_OBTAIN:
		result = pthread_mutex_lock ((pthread_mutex_t*) *mutex);
		break;
	case AV_LOCK_RELEASE:
		result = pthread_mutex_unlock ((pthread_mutex_t*) *mutex);
		break;
	case AV_LOCK_DESTROY:
		result = pthread_mutex_destroy ((pthread_mutex_t*) *mutex);
		free ((pthread_mutex_t*) *mutex);
		*mutex = NULL;
		break;
	default:
		/* We could return -1 here, but examination of the FFmpeg
		 * code shows that return code testing is erratic, so we'll
		 * take charge and complain loudly if FFmpeg/LibAV's API
		 * changes.  This way we don't end up chasing phantoms. */
		fatal ("Unexpected FFmpeg lock request received: %d", op);
	}

	return result;
}

/* Here we attempt to determine if FFmpeg/LibAV has trashed the 'duration'
 * and 'bit_rate' fields in AVFormatContext for large files.  Determining
 * whether or not they are likely to be valid is imprecise and will vary
 * depending (at least) on:
 *
 * - The file's size,
 * - The file's codec,
 * - The number and size of tags,
 * - The version of FFmpeg/LibAV, and
 * - Whether it's FFmpeg or LibAV.
 *
 * This function represents a best guess.
*/
static bool is_timing_broken (AVFormatContext *ic)
{
	if (ic->duration < 0 || ic->bit_rate < 0)
		return true;

	/* If and when FFmpeg uses the right field for its calculation this
	 * should be self-correcting. */
	if (ic->duration < AV_TIME_BASE && !strcmp (ic->iformat->name, "libgme"))
		return true;

	/* AAC timing is inaccurate. */
	if (!strcmp (ic->iformat->name, "aac"))
		return true;

	/* Formats less than 4 GiB should be okay, except those excluded above. */
	if (avio_size (ic->pb) < UINT32_MAX)
		return false;

	/* WAV files are limited to 4 GiB but that doesn't stop some encoders. */
	if (!strcmp (ic->iformat->name, "wav"))
		return true;

	if (!strcmp (ic->iformat->name, "au"))
		return true;

	return false;
}

static int ffmpeg_io_read_cb (void *s, uint8_t *buf, int count)
{
	if (!buf || count == 0)
		return 0;

	return io_read ((struct io_stream *)s, buf, (size_t)count);
}

static int64_t ffmpeg_io_seek_cb (void *s, int64_t offset, int whence)
{
	int w;
	int64_t result = -1;

	/* Warning: Do not blindly accept the avio.h comments for AVSEEK_FORCE
	*          and AVSEEK_SIZE; they are incorrect for later FFmpeg/LibAV
	*          versions. */

	w = whence & ~AVSEEK_FORCE;

	switch (w) {
	case SEEK_SET:
	case SEEK_CUR:
	case SEEK_END:
		result = io_seek ((struct io_stream *)s, offset, w);
		break;
	case AVSEEK_SIZE:
		result = io_file_size ((struct io_stream *)s);
		break;
	}

	return result;
}

static inline int compute_bitrate (sound_params *sound_params,
				int bytes_used, int bytes_produced,
				int bitrate)
{
	int64_t bytes_per_frame, bytes_per_second, seconds;

	bytes_per_frame = sfmt_Bps (sound_params->fmt) * sound_params->channels;
	bytes_per_second = bytes_per_frame * (int64_t)sound_params->rate;
	seconds = (int64_t)bytes_produced / bytes_per_second;
	if (seconds > 0)
		bitrate = (int)((int64_t)bytes_used * 8 / seconds);

	return bitrate;
}

struct ffmpeg_data : public Codec
{
	AVFormatContext *ic;
	AVIOContext *pb;
	AVStream *stream;
	AVCodecContext *enc;
	AVCodec *codec;

	char *remain_buf;
	int remain_buf_len;

	bool delay;             /* FFmpeg may buffer samples */
	bool eof;               /* end of file seen */
	bool eos;               /* end of sound seen */
	bool okay;              /* was this stream successfully opened? */

	char *filename;
	struct io_stream *iostream;
	long fmt;
	int sample_width;
	int bitrate;            /* in bits per second */
	int avg_bitrate;        /* in bits per second */
	#if SEEK_IN_DECODER
	bool seek_req;          /* seek requested */
	int seek_sec;           /* second to which to seek */
	#endif
	bool seek_broken;       /* FFmpeg seeking is broken */
	bool timing_broken;     /* FFmpeg trashes duration and bit_rate */
	#if SEEK_IN_DECODER && defined(DEBUG)
	pthread_t thread_id;
	#endif

	long fmt_from_sample_fmt () const
	{
		switch (enc->sample_fmt) {
		case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  return SFMT_U8;
		case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: return SFMT_S16;
		case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: return SFMT_S32;
		case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: return SFMT_FLOAT;
		default: return 0;
		}
	}

	/* Try to figure out if seeking is broken for this format.
	* The aim here is to try and ensure that seeking either works
	* properly or (because of FFmpeg breakages) is disabled. */
	bool is_seek_broken ()
	{
		/* How much do we trust this? */
		if (!ic->pb->seekable) {
			debug ("Seek broken by AVIOContext.seekable");
			return true;
		}

	#if !SEEK_IN_DECODER
		/* FLV (.flv): av_seek_frame always returns an error (even on success).
		*             Seeking from the decoder works for false errors (but
		*             probably not for real ones) because the player doesn't
		*             get to see them. */
		if (avcodec_version () < AV_VERSION_INT(55,8,100))
		{
			if (!strcmp (ic->iformat->name, "flv"))
				return true;
		}
	#endif

		return false;
	}

	/* Downmix multi-channel audios to stereo. */
	void set_downmixing ()
	{
		if (av_get_channel_layout_nb_channels (enc->channel_layout) <= 2)
			return;

		enc->request_channel_layout = AV_CH_LAYOUT_STEREO;
	}


	ffmpeg_data()
	{
		ic = NULL;
		pb = NULL;
		stream = NULL;
		enc = NULL;
		codec = NULL;
		remain_buf = NULL;
		remain_buf_len = 0;
		delay = false;
		eof = false;
		eos = false;
		okay = false;
		filename = NULL;
		iostream = NULL;
		fmt = 0;
		sample_width = 0;
		bitrate = 0;
		avg_bitrate = 0;
	#if SEEK_IN_DECODER
		seek_req = false;
		seek_sec = 0;
	#endif
		seek_broken = false;
		timing_broken = false;
	#if SEEK_IN_DECODER && defined(DEBUG)
		thread_id = 0;
	#endif
	}

	void ffmpeg_open_internal ()
	{
		int err;
		const char *extn = NULL;
		unsigned int audio_ix;

		ic = avformat_alloc_context ();
		if (!ic) {
			error.fatal("Can't allocate format context!");
			return;
		}

		ic->pb = avio_alloc_context (NULL, 0, 0, iostream,
						ffmpeg_io_read_cb, NULL,
						ffmpeg_io_seek_cb);
		if (!ic->pb)
		{
			error.fatal("Can't allocate avio context!");
			return;
		}

		/* Save AVIO context pointer so we can workaround an FFmpeg
		* memory leak later in ffmpeg_close(). */
		pb = ic->pb;

		err = avformat_open_input (&ic, NULL, NULL, NULL);
		if (err < 0) {
			char *buf = ffmpeg_strerror (err);
			error.fatal("Can't open audio: %s", buf);
			free (buf);
			return;
		}

		/* When FFmpeg and LibAV misidentify a file's codec (and they do)
		* then hopefully this will save MOC from wanton destruction. */
		if (filename) {
			extn = ext_pos (filename);
			if (extn && !strcasecmp (extn, "wav")
				&& strcmp (ic->iformat->name, "wav")) {
				error.fatal(
					"Format possibly misidentified "
					"as '%s' by FFmpeg/LibAV",
					ic->iformat->name);
				goto end;
			}
		}

		err = avformat_find_stream_info (ic, NULL);
		if (err < 0) {
			/* Depending on the particular FFmpeg/LibAV version in use, this
			* may misreport experimental codecs.  Given we don't know the
			* codec at this time, we will have to live with it. */
			char *buf = ffmpeg_strerror (err);
			error.fatal("Could not find codec parameters: %s", buf);
			free (buf);
			goto end;
		}

		audio_ix = find_first_audio (ic);
		if (audio_ix == ic->nb_streams) {
			error.fatal("No audio in source");
			goto end;
		}

		stream = ic->streams[audio_ix];
		enc = stream->codec;

		codec = avcodec_find_decoder (enc->codec_id);
		if (!codec) {
			error.fatal("No codec for this audio");
			goto end;
		}

		if (filename) {
			const char *fn;

			fn = strrchr (filename, '/');
			fn = fn ? fn + 1 : filename;
			debug ("FFmpeg thinks '%s' is format(codec) '%s(%s)'",
				fn, ic->iformat->name, codec->name);
		}
		else
			debug ("FFmpeg thinks stream is format(codec) '%s(%s)'",
				ic->iformat->name, codec->name);

		/* This may or may not work depending on the particular version of
		* FFmpeg/LibAV in use.  For some versions this will be caught in
		* *_find_stream_info() above and misreported as an unfound codec
		* parameters error. */
		if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
			error.fatal("The codec is experimental and may damage MOC: %s", codec->name);
			goto end;
		}

		set_downmixing();
		if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
			enc->flags |= AV_CODEC_FLAG_TRUNCATED;

		if (avcodec_open2 (enc, codec, NULL) < 0)
		{
			error.fatal("No codec for this audio");
			goto end;
		}

		fmt = fmt_from_sample_fmt ();
		if (fmt == 0) {
			error.fatal(
				"Cannot get sample size from unknown sample format: %s",
				av_get_sample_fmt_name (enc->sample_fmt));
			avcodec_close (enc);
			goto end;
		}

		sample_width = sfmt_Bps (fmt);

		if (codec->capabilities & AV_CODEC_CAP_DELAY)
			delay = true;
		seek_broken = is_seek_broken ();
		timing_broken = is_timing_broken (ic);

		if (timing_broken && extn && !strcasecmp (extn, "wav")) {
			error.fatal("Broken WAV file; use W64!");
			avcodec_close (enc);
			goto end;
		}

		okay = true;

		if (!timing_broken && ic->duration >= AV_TIME_BASE)
			avg_bitrate = (int) (avio_size (ic->pb) /
						(ic->duration / AV_TIME_BASE) * 8);

		if (!timing_broken && ic->bit_rate > 0)
			bitrate = ic->bit_rate;

		return;

	end:
		avformat_close_input (&ic);
	}

	void put_in_remain_buf (const char *buf, const int len)
	{
		remain_buf_len = len;
		remain_buf = (char *)xmalloc (len);
		memcpy (remain_buf, buf, len);
	}

	void add_to_remain_buf (const char *buf, const int len)
	{
		remain_buf = (char *)xrealloc (remain_buf, remain_buf_len + len);
		memcpy (remain_buf + remain_buf_len, buf, len);
		remain_buf_len += len;
	}

	/* Free the remainder buffer. */
	void free_remain_buf ()
	{
		free (remain_buf);
		remain_buf = NULL;
		remain_buf_len = 0;
	}

	/* Satisfy the request from previously decoded samples. */
	int take_from_remain_buf (char *buf, int buf_len)
	{
		int to_copy = MIN (buf_len, remain_buf_len);

		memcpy (buf, remain_buf, to_copy);

		if (to_copy < remain_buf_len) {
			memmove (remain_buf, remain_buf + to_copy,
					remain_buf_len - to_copy);
			remain_buf_len -= to_copy;
		}
		else {
			free_remain_buf ();
		}

		return to_copy;
	}

	/* Copy samples to output or remain buffer. */
	int copy_or_buffer (char *in, int in_len, char *out, int out_len)
	{
		if (in_len == 0)
			return 0;

		if (in_len <= out_len) {
			memcpy (out, in, in_len);
			return in_len;
		}

		if (out_len == 0) {
			add_to_remain_buf (in, in_len);
			return 0;
		}

		memcpy (out, in, out_len);
		put_in_remain_buf (in + out_len, in_len - out_len);
		return out_len;
	}

	/* Create a new packet ('cause FFmpeg doesn't provide one). */
	inline AVPacket *new_packet ()
	{
		assert (stream);
		AVPacket *pkt = av_packet_alloc ();
		pkt->stream_index = stream->index;
		return pkt;
	}

	/* Read a packet from the file or empty packet if flushing delayed
	* samples. */
	AVPacket *get_packet ()
	{
		int rc;
		AVPacket *pkt;

		assert (!eos);

		pkt = new_packet ();

		if (eof)
			return pkt;

		rc = av_read_frame (ic, pkt);
		if (rc >= 0) {
			return pkt;
		}

		av_packet_free (&pkt);

		/* FFmpeg has (at least) two ways of indicating EOF.  (Awesome!) */
		if (rc == AVERROR_EOF)
			eof = true;
		if (ic->pb && ic->pb->eof_reached)
			eof = true;

		if (!eof && rc < 0) {
			char *buf = ffmpeg_strerror (rc);
			error.fatal("Error in the stream: %s", buf);
			free (buf);
			return NULL;
		}

		if (delay)
			return new_packet ();

		eos = true;
		return NULL;
	}

	/* Decode samples from packet data. */
	int decode_packet (AVPacket *pkt, char *buf, int buf_len)
	{
		int filled = 0;
		char *packed;
		AVFrame *frame;

		frame = av_frame_alloc ();

		do {
			int len, got_frame, is_planar, packed_size, copied;

			len = avcodec_decode_audio4 (enc, frame, &got_frame, pkt);

			if (len < 0) {
				/* skip frame */
				error.warn("Error in the stream!");
				break;
			}

			pkt->data += len;
			pkt->size -= len;

			if (!got_frame) {
				eos = eof && (pkt->size == 0);
				continue;
			}

			if (frame->nb_samples == 0)
				continue;

			is_planar = av_sample_fmt_is_planar (enc->sample_fmt);
			packed = (char *)frame->extended_data[0];
			packed_size = frame->nb_samples * sample_width
							* enc->channels;

			if (is_planar && enc->channels > 1) {
				int sample, ch;

				packed = (char*) xmalloc (packed_size);

				for (sample = 0; sample < frame->nb_samples; sample += 1) {
					for (ch = 0; ch < enc->channels; ch += 1)
						memcpy (packed + (sample * enc->channels + ch)
									* sample_width,
							(char *)frame->extended_data[ch] + sample * sample_width,
							sample_width);
				}
			}

			copied = copy_or_buffer (packed, packed_size, buf, buf_len);
			buf += copied;
			filled += copied;
			buf_len -= copied;

			if (packed != (char *)frame->extended_data[0])
				free (packed);
		} while (pkt->size > 0);

		av_frame_free (&frame);

		return filled;
	}

	#if SEEK_IN_DECODER
	bool seek_in_stream ()
	#else
	bool seek_in_stream (int sec)
	#endif
	{
		int rc, flags = AVSEEK_FLAG_ANY;
		int64_t seek_ts;

	#if SEEK_IN_DECODER
		int sec = seek_sec;

	#ifdef DEBUG
		assert (pthread_equal (thread_id, pthread_self ()));
	#endif
	#endif

		/* FFmpeg can't seek if the file has already reached EOF. */
		if (eof)
			return false;

		seek_ts = av_rescale (sec, stream->time_base.den,
					stream->time_base.num);

		if (stream->start_time != (int64_t)AV_NOPTS_VALUE) {
			if (seek_ts > INT64_MAX - stream->start_time) {
				logit ("Seek value too large");
				return false;
			}
			seek_ts += stream->start_time;
		}

		if (stream->cur_dts > seek_ts)
			flags |= AVSEEK_FLAG_BACKWARD;

		rc = av_seek_frame (ic, stream->index, seek_ts, flags);
		if (rc < 0) {
			log_errno ("Seek error", rc);
			return false;
		}

		avcodec_flush_buffers (stream->codec);

		return true;
	}

	int decode(char *buf, int buf_len, sound_params &sound_params) override
	{
		int bytes_used = 0, bytes_produced = 0;

		error.clear();

		if (eos) return 0;

		/* FFmpeg claims to always return native endian. */
		sound_params.channels = enc->channels;
		sound_params.rate = enc->sample_rate;
		sound_params.fmt = fmt | SFMT_NE;

	#if SEEK_IN_DECODER
		if (seek_req) {
			seek_req = false;
			if (seek_in_stream ())
				free_remain_buf ();
		}
	#endif

		if (remain_buf)
			return take_from_remain_buf (buf, buf_len);

		do {
			uint8_t *saved_pkt_data_ptr;
			AVPacket *pkt;

			pkt = get_packet ();
			if (!pkt)
				break;

			if (pkt->stream_index != stream->index) {
				av_packet_free (&pkt);
				continue;
			}

	#ifdef AV_PKT_FLAG_CORRUPT
			if (pkt->flags & AV_PKT_FLAG_CORRUPT) {
				debug ("Dropped corrupt packet.");
				av_packet_free (&pkt);
				continue;
			}
	#endif

			saved_pkt_data_ptr = pkt->data;
			bytes_used += pkt->size;

			bytes_produced = decode_packet (pkt, buf, buf_len);
			buf += bytes_produced;
			buf_len -= bytes_produced;

			/* FFmpeg will segfault if the data pointer is not restored. */
			pkt->data = saved_pkt_data_ptr;
			av_packet_free (&pkt);
		} while (!bytes_produced && !eos);

		if (!timing_broken)
			bitrate = compute_bitrate (&sound_params, bytes_used,
							bytes_produced + remain_buf_len,
							bitrate);

		return bytes_produced;
	}

	int seek (int sec) override
	{
		assert (sec >= 0);

		if (seek_broken) return -1;

	#if SEEK_IN_DECODER
		seek_sec = sec;
		seek_req = true;
	#ifdef DEBUG
		thread_id = pthread_self();
	#endif
	#else

		if (!seek_in_stream (sec)) return -1;
		free_remain_buf ();
	#endif
		return sec;
	}

	~ffmpeg_data()
	{
		/* We need to delve into the AVIOContext struct to free the
		* buffer FFmpeg leaked if avformat_open_input() failed.  Do
		* not be tempted to call avio_close() here; it will segfault. */
		if (pb) {
			av_freep (&pb->buffer);
			av_freep (&pb);
		}

		if (okay) {
			avcodec_close (enc);
			avformat_close_input (&ic);
			free_remain_buf ();
		}

		if (iostream) {
			io_close (iostream);
			iostream = NULL;
		}

		free (filename);
	}

	io_stream *get_stream() override
	{
		return iostream;
	}

	int get_bitrate () const override
	{
		return timing_broken ? -1 : bitrate / 1000;
	}

	int get_avg_bitrate () const override
	{
		return timing_broken ? -1 : avg_bitrate / 1000;
	}

	int get_duration () const override
	{
		if (timing_broken) return -1;
		if (!stream) return -1;
		if (stream->duration == (int64_t)AV_NOPTS_VALUE) return -1;
		if (stream->duration < 0) return -1;
		return stream->duration * stream->time_base.num / stream->time_base.den;
	}
};

struct ffmpeg_decoder : public Decoder
{
	ffmpeg_decoder()
	{
		int rc;

		avcodec_register_all ();
		av_register_all ();

		load_audio_extns ();
		load_video_extns ();

		rc = av_lockmgr_register (locking_cb);
		if (rc < 0) {
			char buf[128];

			av_strerror (rc, buf, sizeof (buf));
			fatal ("Lock manager initialisation failed: %s", buf);
		}
	}

	~ffmpeg_decoder()
	{
		av_lockmgr_register (NULL);
		av_log_set_level (AV_LOG_QUIET);
		supported_extns.clear();
	}

	/* Fill info structure with data from ffmpeg comments. */
	void read_tags(const str &file_name, file_tags &info) override
	{
		int err;
		AVFormatContext *ic = NULL;
		AVDictionaryEntry *entry;
		AVDictionary *md;

		err = avformat_open_input (&ic, file_name.c_str(), NULL, NULL);
		if (err < 0) {
			log_errno ("avformat_open_input() failed", err);
			return;
		}

		err = avformat_find_stream_info (ic, NULL);
		if (err < 0) {
			log_errno ("avformat_find_stream_info() failed", err);
			goto end;
		}

		if (!is_timing_broken (ic)) {
			info.time = -1;
			if (ic->duration != (int64_t)AV_NOPTS_VALUE && ic->duration >= 0)
				info.time = ic->duration / AV_TIME_BASE;
		}

		md = ic->metadata;
		if (md == NULL) {
			unsigned int audio_ix;

			audio_ix = find_first_audio (ic);
			if (audio_ix < ic->nb_streams)
				md = ic->streams[audio_ix]->metadata;
		}

		if (md == NULL) {
			debug ("no metadata found");
			goto end;
		}

		entry = av_dict_get (md, "track", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info.track = atoi (entry->value);
		entry = av_dict_get (md, "title", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info.title = entry->value;
		entry = av_dict_get (md, "artist", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info.artist = entry->value;
		entry = av_dict_get (md, "album", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info.album = entry->value;

	end:
		avformat_close_input (&ic);
	}

	Codec* open(const str &file) override
	{
		ffmpeg_data *data = new ffmpeg_data;

		data->filename = xstrdup (file.c_str());
		data->iostream = io_open (file.c_str(), 1);
		if (!io_ok (data->iostream)) {
			data->error.fatal("Can't open file: %s", io_strerror(data->iostream));
			return data;
		}

		data->ffmpeg_open_internal();
		return data;
	}

	Codec* open(io_stream &stream) override
	{
		ffmpeg_data *data = new ffmpeg_data;
		data->iostream = &stream;
		data->ffmpeg_open_internal();
		return data;
	}

	bool can_decode(io_stream &stream) override
	{
		int res;
		AVProbeData probe_data;
		AVInputFormat *fmt;
		char buf[8096 + AVPROBE_PADDING_SIZE] = {0};

		res = io_peek (&stream, buf, sizeof (buf));
		if (res < 0) {
			error ("Stream error: %s", io_strerror(&stream));
			return 0;
		}

		probe_data.filename = NULL;
		probe_data.buf = (unsigned char*)buf;
		probe_data.buf_size = sizeof (buf) - AVPROBE_PADDING_SIZE;
		probe_data.mime_type = NULL;

		fmt = av_probe_input_format (&probe_data, 1);

		return fmt != NULL;
	}

	bool matches_ext(const char *ext) const override
	{
		return supported_extns.count(ext);
	}

	bool matches_mime (const str &mime_type) const override
	{
		return av_guess_format (NULL, NULL, mime_type.c_str());
	}
};

Decoder *ffmpeg_plugin ()
{
	return new ffmpeg_decoder;
}
