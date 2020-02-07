/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * libwavpack-plugin Copyright (C) 2006 Alexandrov Sergey <splav@unsorted.ru>
 * Enables MOC to play wavpack files (actually just a wrapper around
 * wavpack library).
 *
 * Structure of this plugin is an adaption of the libvorbis-plugin from
 * moc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <wavpack/wavpack.h>

#include "decoder.h" /* required: provides decoder structure definition */
#include "io.h" /* if you use io_*() functions to access files. */
#include "../audio.h" /* for sound_params structure */

struct wavpack_data : public Codec
{
	WavpackContext *wpc;
	int sample_num;
	int sample_rate;
	int avg_bitrate;
	int channels;
	int duration;
	int mode;

	wavpack_data(const char *file) : wpc(NULL)
	{
		int o_flags = OPEN_2CH_MAX | OPEN_WVC;

		char wv_error[100];

		if ((wpc = WavpackOpenFileInput(file, wv_error, o_flags, 0)) == NULL) {
			error.fatal("%s", wv_error);
			logit ("wv_open error: %s", wv_error);
			return;
		}

		sample_num = WavpackGetNumSamples (wpc);
		sample_rate = WavpackGetSampleRate (wpc);
		channels = WavpackGetReducedChannels (wpc);
		duration = sample_num / sample_rate;
		mode = WavpackGetMode (wpc);
		avg_bitrate = WavpackGetAverageBitrate (wpc, 1) / 1000;
	}

	~wavpack_data()
	{
		if (wpc) WavpackCloseFile (wpc);
	}

	int seek (int sec) override
	{
		if (WavpackSeekSample (wpc, sec * sample_rate)) return sec;
		error.fatal("Fatal seeking error!");
		return -1;
	}

	int get_bitrate () const override
	{
		int bitrate = WavpackGetInstantBitrate (wpc) / 1000;
		return (bitrate == 0)? avg_bitrate : bitrate;
	}

	int get_avg_bitrate () const override
	{
		return avg_bitrate;
	}

	int get_duration () const override
	{
		return duration;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		int ret, i, s_num, iBps, oBps;

		int8_t *buf8 = (int8_t *)buf;
		int16_t *buf16 = (int16_t *)buf;
		int32_t *buf32 = (int32_t *)buf;

		iBps = channels * WavpackGetBytesPerSample (wpc);
		oBps = (iBps == 6) ? 8 : iBps;
		s_num = buf_len / oBps;

		error.clear();

		int32_t *dbuf = (int32_t *)xcalloc (s_num, channels * 4);

		ret = WavpackUnpackSamples (wpc, dbuf, s_num);

		if (ret == 0) {
			free (dbuf);
			return 0;
		}

		if (mode & MODE_FLOAT) {
			sound_params.fmt = SFMT_FLOAT;
			memcpy (buf, dbuf, ret * oBps);
		} else	{
			debug ("iBps %d", iBps);
			switch (iBps / channels){
			case 4: for (i = 0; i < ret * channels; i++) buf32[i] = dbuf[i];
				sound_params.fmt = SFMT_S32 | SFMT_NE;
				break;
			case 3: for (i = 0; i < ret * channels; i++) buf32[i] = dbuf[i] * 256;
				sound_params.fmt = SFMT_S32 | SFMT_NE;
				break;
			case 2: for (i = 0; i < ret * channels; i++) buf16[i] = dbuf[i];
				sound_params.fmt = SFMT_S16 | SFMT_NE;
				break;
			case 1: for (i = 0; i < ret * channels; i++) buf8[i] = dbuf[i];
				sound_params.fmt = SFMT_S8 | SFMT_NE;
			}
		}

		sound_params.channels = channels;
		sound_params.rate = sample_rate;

		free (dbuf);
		return ret * oBps ;
	}
};

struct wavpack_decoder : public Decoder
{
	bool matches_ext(const char *ext) const override
	{
		return !strcasecmp (ext, "WV");
	}

	void read_tags(const str &file_name, file_tags &info)
	{
		char wv_error[100];
		WavpackContext *wpc = WavpackOpenFileInput (file_name.c_str(), wv_error, OPEN_TAGS, 0);
		if (!wpc) {
			logit ("wv_open error: %s", wv_error);
			return;
		}

		int duration = WavpackGetNumSamples (wpc) / WavpackGetSampleRate (wpc);
		info.time = duration;

		int tag_len;
		if ((tag_len = WavpackGetTagItem (wpc, "title", NULL, 0)) > 0) {
			std::vector<char> buf(++tag_len); char *s = (char*)buf.data();
			WavpackGetTagItem (wpc, "title", s, tag_len);
			info.title = s;
		}

		if ((tag_len = WavpackGetTagItem (wpc, "artist", NULL, 0)) > 0) {
			std::vector<char> buf(++tag_len); char *s = (char*)buf.data();
			WavpackGetTagItem (wpc, "artist", s, tag_len);
			info.artist = s;
		}

		if ((tag_len = WavpackGetTagItem (wpc, "album", NULL, 0)) > 0) {
			std::vector<char> buf(++tag_len); char *s = (char*)buf.data();
			WavpackGetTagItem (wpc, "album", s, tag_len);
			info.album = s;
		}

		if ((tag_len = WavpackGetTagItem (wpc, "track", NULL, 0)) > 0) {
			std::vector<char> buf(++tag_len); char *s = (char*)buf.data();
			WavpackGetTagItem (wpc, "track", s, tag_len);
			info.track = atoi (s);
		}

		WavpackCloseFile (wpc);
	}
};

Decoder *wav_plugin ()
{
	return new wavpack_decoder;
}
