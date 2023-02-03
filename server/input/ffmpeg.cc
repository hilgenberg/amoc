#include "../audio.h"
#include "decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
}

static std::set<std::string> supported_extns;

/* FFmpeg-provided error code to description function wrapper. */
static inline char *ffmpeg_strerror (int errnum)
{
	static char buf[256];
	av_strerror (errnum, buf, 256);
	buf[255] = 0;
	return buf;
}

static void load_extns ()
{
	/* When adding an entry to this list, tests need to be performed to
	 * determine whether or not FFmpeg/LibAV handles durations and seeking
	 * correctly.  If not, then the appropriate additions should be made
	 * in is_timing_broken() and is_seek_broken(). */
	struct extn_list { const char *extn, *format; };
	const struct extn_list extns[] = {
		// audio extensions:
		{"aac",  "aac"},	{"ac3",  "ac3"},	{"ape",  "ape"},
		{"au",   "au"}, 	{"ay",   "libgme"},	{"dff",  "dsf"},
		{"dsf",  "dsf"},	{"dts",  "dts"},	{"eac3", "eac3"},
		{"fla",  "flac"},	{"flac", "flac"},	{"gbs",  "libgme"},
		{"gym",  "libgme"},	{"hes",  "libgme"},	{"kss",  "libgme"},
		{"mka",  "matroska"},	{"mp2",  "mpeg"},	{"mp3",  "mp3"},
		{"mpc",  "mpc"},	{"mpc8", "mpc8"},	{"m4a",  "m4a"},
		{"nsf",  "libgme"},	{"nsfe", "libgme"},	{"ra",   "rm"},
		{"sap",  "libgme"},	{"spc",  "libgme"},	{"tta",  "tta"},
		{"vgm",  "libgme"},	{"vgz",  "libgme"},	{"vqf",  "vqf"},
		{"wav",  "wav"},	{"w64",  "w64"},	{"wma",  "asf"},
		{"wv",   "wv"},
		// video extensions:
		{"avi",  "avi"},	{"flv",  "flv"},	{"mkv",  "matroska"},
		{"mp4",  "mp4"},	{"rec",  "mpegts"},	{"vob",  "mpeg"},
		{"webm", "matroska"},
		{NULL, NULL}
	};

	for (int i = 0; extns[i].extn; ++i) {
		if (av_find_input_format (extns[i].format))
			supported_extns.insert(extns[i].extn);
	}

	if (av_find_input_format ("ogg")) {
		supported_extns.insert("ogg");
		if (avcodec_find_decoder(AV_CODEC_ID_VORBIS)) supported_extns.insert("oga");
		if (avcodec_find_decoder(AV_CODEC_ID_OPUS))   supported_extns.insert("opus");
		if (avcodec_find_decoder(AV_CODEC_ID_THEORA)) supported_extns.insert("ogv");
	}
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
	if (ic->duration < 0 || ic->bit_rate < 0) return true;

	/* If and when FFmpeg uses the right field for its calculation this
	 * should be self-correcting. */
	if (ic->duration < AV_TIME_BASE && !strcmp (ic->iformat->name, "libgme"))
		return true;

	/* AAC timing is inaccurate. */
	if (!strcmp (ic->iformat->name, "aac")) return true;

	/* Formats less than 4 GiB should be okay, except those excluded above. */
	if (avio_size (ic->pb) < UINT32_MAX) return false;

	/* WAV files are limited to 4 GiB but that doesn't stop some encoders. */
	if (!strcmp (ic->iformat->name, "wav")) return true;

	if (!strcmp (ic->iformat->name, "au")) return true;

	return false;
}

static int ffmpeg_io_read_cb (void *s, uint8_t *buf, int count)
{
	if (!buf || count == 0) return 0;
	return io_read ((struct io_stream *)s, buf, (size_t)count);
}

static int64_t ffmpeg_io_seek_cb (void *s, int64_t offset, int whence)
{
	/* Warning: Do not blindly accept the avio.h comments for AVSEEK_FORCE
	*          and AVSEEK_SIZE; they are incorrect for later FFmpeg/LibAV
	*          versions. */
	int w = whence & ~AVSEEK_FORCE;
	switch (w) {
	case SEEK_SET:
	case SEEK_CUR:
	case SEEK_END:    return io_seek ((struct io_stream *)s, offset, w);
	case AVSEEK_SIZE: return io_file_size ((struct io_stream *)s);
	default:          return -1;
	}
}

struct ffmpeg_data : public Codec
{
	AVFormatContext *ic = NULL;
	AVIOContext *pb = NULL;
	AVStream *stream = NULL;
	AVCodecParameters *enc = NULL;
	const AVCodec *codec = NULL;
	AVCodecContext *context = NULL;
	AVFrame *frame = NULL;

	char *remain_buf = NULL;
	int remain_buf_len = 0;

	bool delay = false;             /* FFmpeg may buffer samples */
	bool eof = false;               /* end of file seen */
	bool eos = false;               /* end of sound seen */
	bool okay = false;              /* was this stream successfully opened? */

	char *filename = NULL;
	struct io_stream *iostream = NULL;
	long fmt = 0;
	int sample_width = 0;
	int bitrate = 0;            /* in bits per second */
	int avg_bitrate = 0;        /* in bits per second */
	int64_t cur_dts = 0; // for setting AVSEEK_FLAG_BACKWARD :-/
	bool seek_broken = false;       /* FFmpeg seeking is broken */
	bool timing_broken = false;     /* FFmpeg trashes duration and bit_rate */

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

		/* FLV (.flv): av_seek_frame always returns an error (even on success).
		*             Seeking from the decoder works for false errors (but
		*             probably not for real ones) because the player doesn't
		*             get to see them. */
		if (avcodec_version () < AV_VERSION_INT(55,8,100) && !strcmp (ic->iformat->name, "flv"))
			return true;

		return false;
	}

	void ffmpeg_open_internal ()
	{
		ic = avformat_alloc_context ();
		if (!ic) {
			error.fatal("Can't allocate format context!");
			return;
		}

		ic->pb = avio_alloc_context (NULL, 0, 0, iostream,
						ffmpeg_io_read_cb, NULL,
						ffmpeg_io_seek_cb);
		if (!ic->pb) {
			error.fatal("Can't allocate avio context!");
			return;
		}

		/* Save AVIO context pointer so we can workaround an FFmpeg
		* memory leak later in ffmpeg_close(). */
		pb = ic->pb;

		int err = avformat_open_input (&ic, NULL, NULL, NULL);
		if (err < 0) {
			error.fatal("Can't open audio: %s", ffmpeg_strerror (err));
			return;
		}

		/* When FFmpeg and LibAV misidentify a file's codec (and they do)
		* then hopefully this will save MOC from wanton destruction. */
		const char *extn = NULL;
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
			error.fatal("Could not find codec parameters: %s", ffmpeg_strerror (err));
			goto end;
		}

		int audio_ix; audio_ix = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (audio_ix < 0) {
			error.fatal("No audio in source");
			goto end;
		}
		stream = ic->streams[audio_ix];
		enc = stream->codecpar;

		codec = avcodec_find_decoder (enc->codec_id);
		if (!codec) {
			error.fatal("No codec for this audio");
			goto end;
		}

		if (filename) {
			const char *fn = strrchr (filename, '/');
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

		if (av_get_channel_layout_nb_channels (enc->channel_layout) > 2)
			context->request_channel_layout = AV_CH_LAYOUT_STEREO;

		context = avcodec_alloc_context3(codec);
		if (!context) {
			error.fatal("Failed to allocate codec context");
			goto end;
		}

		// Copy the codec parameters into the context
		if (avcodec_parameters_to_context(context, enc) < 0) {
			error.fatal("Failed to copy codec parameters to codec context");
			goto end;
		}

		// Open the codec
		if (avcodec_open2(context, codec, nullptr) < 0)
		{
			error.fatal("No codec for this audio");
			goto end;
		}

		switch (context->sample_fmt)
		{
			case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  fmt = SFMT_U8; break;
			case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: fmt = SFMT_S16; break;
			case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: fmt = SFMT_S32; break;
			case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: fmt = SFMT_FLOAT; break;
			default:
				error.fatal("Cannot get sample size from unknown sample format: %s",
					av_get_sample_fmt_name (context->sample_fmt));
				avcodec_close(context);
				goto end;
		}

		sample_width = sfmt_Bps (fmt);

		if (codec->capabilities & AV_CODEC_CAP_DELAY) delay = true;
		seek_broken = is_seek_broken ();
		timing_broken = is_timing_broken (ic);

		if (timing_broken && extn && !strcasecmp (extn, "wav")) {
			error.fatal("Broken WAV file; use W64!");
			avcodec_close(context);
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

	/* Free the remainder buffer. */
	void free_remain_buf ()
	{
		free (remain_buf);
		remain_buf = NULL;
		remain_buf_len = 0;
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

		if (remain_buf)
		{
			if (remain_buf_len <= buf_len)
			{
				const int n = remain_buf_len;
				memcpy(buf, remain_buf, n);
				free_remain_buf ();
				return n;
			}
			memcpy(buf, remain_buf, buf_len);
			memmove(remain_buf, remain_buf + buf_len, remain_buf_len - buf_len);
			remain_buf_len -= buf_len;
			return buf_len;
		}

		do {
			/* Read a packet from the file or empty packet if flushing delayed
			* samples. */
			assert (!eos);

			AVPacket *pkt = av_packet_alloc ();
			pkt->stream_index = stream->index;
			int rc = 0;
			if (!eof) rc = av_read_frame (ic, pkt); // use context instead of ic?
			if (rc < 0)
			{
				/* FFmpeg has (at least) two ways of indicating EOF.  (Awesome!) */
				if (rc == AVERROR_EOF || (ic->pb && ic->pb->eof_reached))
					eof = true;

				if (!eof) {
					error.fatal("Error in the stream: %s", ffmpeg_strerror (rc));
					av_packet_free (&pkt);
					break;
				}

				if (!delay)
				{
					av_packet_free (&pkt);
					eos = true;
					break;
				}
			}

			if (pkt->stream_index != stream->index || pkt->size <= 0) {
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

			uint8_t *saved_pkt_data_ptr = pkt->data;
			bytes_used += pkt->size;

			if (!frame) frame = av_frame_alloc ();

			// Pass our compressed data into the codec
			rc = avcodec_send_packet(context, pkt);
			if (rc != 0) {
				error.fatal("avcodec_send_packet error: %s", ffmpeg_strerror (rc));
				break;
			}

			// Retrieve our raw data from the codec
			rc = avcodec_receive_frame(context, frame);
			if (rc == AVERROR(EAGAIN)) {
				// The codec needs more data before it can decode
				error.warn("avcodec_receive_frame returned EAGAIN");
				av_packet_free (&pkt);
				continue;
			} else if (rc != 0) {
				error.fatal("avcodec_receive_frame error: %s", ffmpeg_strerror (rc));
				av_packet_free (&pkt);
				break;
			}

			if (frame->nb_samples <= 0)
			{
				av_packet_free (&pkt);
				continue;
			}

			bool is_planar = av_sample_fmt_is_planar(context->sample_fmt);
			char *packed = (char *)frame->data;
			int packed_size = frame->nb_samples * sample_width * enc->channels;

			if (is_planar && enc->channels > 1) {
				packed = (char*) xmalloc (packed_size);

				for (int sample = 0; sample < frame->nb_samples; ++sample) {
					for (int ch = 0; ch < enc->channels; ++ch)
						memcpy (packed + (sample * enc->channels + ch) * sample_width,
							(char *)frame->extended_data[ch] + sample * sample_width,
							sample_width);
				}
			}

			int copied = std::min(buf_len, packed_size);
			memcpy(buf, packed, copied);
			if (packed_size > copied)
			{
				remain_buf_len = packed_size - copied;
				remain_buf = (char *)xmalloc (remain_buf_len);
				memcpy (remain_buf, packed + copied, remain_buf_len);
			}

			cur_dts = pkt->dts;

			buf            += copied;
			buf_len        -= copied;
			bytes_produced += copied;

			if (packed != (char *)frame->extended_data[0]) free (packed);

			av_frame_free (&frame);

			/* FFmpeg will segfault if the data pointer is not restored. */
			pkt->data = saved_pkt_data_ptr;
			av_packet_free (&pkt);
		} while (!bytes_produced && !eos);

		if (!timing_broken)
		{
			// update bitrate
			int64_t bytes_per_frame = sfmt_Bps(sound_params.fmt) * sound_params.channels;
			int64_t bytes_per_second = bytes_per_frame * (int64_t)sound_params.rate;
			int64_t seconds = (int64_t)(bytes_produced + remain_buf_len) / bytes_per_second;
			if (seconds > 0) bitrate = (int)((int64_t)bytes_used * 8 / seconds);
		}

		return bytes_produced;
	}

	int seek(int sec) override
	{
		assert (sec >= 0);
		if (seek_broken) return -1;
		if (eof) return -1; /* FFmpeg can't seek if the file has already reached EOF. */

		int64_t seek_ts = av_rescale (sec, stream->time_base.den, stream->time_base.num);

		if (stream->start_time != (int64_t)AV_NOPTS_VALUE) {
			if (seek_ts > INT64_MAX - stream->start_time) {
				logit ("Seek value too large");
				return -1;
			}
			seek_ts += stream->start_time;
		}

		int flags = AVSEEK_FLAG_ANY;
		if (cur_dts > seek_ts) flags |= AVSEEK_FLAG_BACKWARD;
		int rc = av_seek_frame (ic, stream->index, seek_ts, flags);
		if (rc < 0) { log_errno ("Seek error", rc); return -1; }

		avcodec_flush_buffers (context);
		free_remain_buf ();
		return sec;
	}

	~ffmpeg_data()
	{
		if (frame) av_frame_free(&frame);

		/* We need to delve into the AVIOContext struct to free the
		* buffer FFmpeg leaked if avformat_open_input() failed.  Do
		* not be tempted to call avio_close() here; it will segfault. */
		if (pb) {
			av_freep (&pb->buffer);
			av_freep (&pb);
		}

		if (okay) {
			avcodec_close (context);
			avformat_close_input (&ic);
			free_remain_buf ();
		}
		if (context) avcodec_free_context(&context);

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
		load_extns ();
	}

	~ffmpeg_decoder()
	{
		av_log_set_level (AV_LOG_QUIET);
		supported_extns.clear();
	}

	/* Fill info structure with data from ffmpeg comments. */
	void read_tags(const str &file_name, file_tags &info) override
	{
		int err;
		AVFormatContext *ic = NULL;
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
		if (!md) {
			int audio_ix = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
			if (audio_ix >= 0 && audio_ix < ic->nb_streams)
				md = ic->streams[audio_ix]->metadata;
		}

		if (!md) {
			debug ("no metadata found");
			goto end;
		}

		AVDictionaryEntry *e;
		e = av_dict_get(md, "track",  NULL, 0); if (e && e->value && *e->value) info.track  = atoi(e->value);
		e = av_dict_get(md, "title",  NULL, 0); if (e && e->value && *e->value) info.title  = e->value;
		e = av_dict_get(md, "artist", NULL, 0); if (e && e->value && *e->value) info.artist = e->value;
		e = av_dict_get(md, "album",  NULL, 0); if (e && e->value && *e->value) info.album  = e->value;

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
		AVProbeData probe_data;
		char buf[8096 + AVPROBE_PADDING_SIZE] = {0};

		int res = io_peek (&stream, buf, sizeof (buf));
		if (res < 0) {
			error ("Stream error: %s", io_strerror(&stream));
			return 0;
		}

		probe_data.filename = NULL;
		probe_data.buf = (unsigned char*)buf;
		probe_data.buf_size = sizeof (buf) - AVPROBE_PADDING_SIZE;
		probe_data.mime_type = NULL;

		return av_probe_input_format (&probe_data, 1) != NULL;
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
