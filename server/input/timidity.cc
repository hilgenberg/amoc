/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * libTiMidity-plugin Copyright (C) 2007 Hendrik Iben <hiben@tzi.de>
 * The hard work is done by the libTiMidity-Library written by
 * Konstantin Korikov (http://libtimidity.sourceforge.net).
 * I have merely hacked together a wrapper...
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <timidity.h>

#include "io.h"
#include "decoder.h"

// former options:
static int TiMidity_Rate = 44100;
static int TiMidity_Bits = 16;
static int TiMidity_Channels = 2;
static int TiMidity_Volume = 100;

MidSongOptions midioptions;

struct timidity_data : public Codec
{
	MidSong *midisong;
	int length;

	timidity_data(const char *file)
	{
		midisong = NULL;
		length = -1000;

		MidIStream *midistream = mid_istream_open_file(file);

		if (!midistream) {
			error.fatal("Can't open midifile: %s", file);
			return;
		}

		midisong = mid_song_load(midistream, &midioptions);
		mid_istream_close(midistream);

		if (!midisong) {
			error.fatal("Can't load midifile: %s", file);
			return;
		}

		length = mid_song_get_total_time(midisong);
	}

	int get_duration() const override { return length/1000; }

	~timidity_data()
	{
		if (midisong) mid_song_free(midisong);
	}

	int seek (int sec) override
	{
		int ms = sec*1000;
		ms = MIN(ms, length);
		mid_song_seek(midisong, ms);
		return ms/1000;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		sound_params.channels = midioptions.channels;
		sound_params.rate = midioptions.rate;
		sound_params.fmt = (midioptions.format==MID_AUDIO_S16LSB)?(SFMT_S16 | SFMT_LE):SFMT_S8;

		return mid_song_read_wave(midisong, (sint8*)buf, buf_len);
	}
};

struct timidity_decoder : public Decoder
{
	bool matches_ext(const char *ext) const override
	{
		return !strcasecmp (ext, "mid");
	}

	bool matches_mime (const str &mime)
	{
		return !strcasecmp(mime.c_str(), "audio/midi")
		|| !strncasecmp(mime.c_str(), "audio/midi;", 10);
	}

	~timidity_decoder()
	{
		mid_exit();
	}

	Codec* open(const str &file) override
	{
		auto *data = new timidity_data(file.c_str());
		if (data->midisong) {
			mid_song_set_volume(data->midisong, TiMidity_Volume);
			mid_song_start(data->midisong);
		}
		return data;
	}

	int get_duration(const str &file_name)
	{
		timidity_data data(file_name.c_str());
		if (data.midisong) return data.get_duration();
		return -1;
	}

	timidity_decoder ()
	{
		int initresult;

		str config = options::TiMidity_Config;
		if (config.empty() || config == "yes")
			initresult = mid_init(NULL);
		else if (config == "no")
			initresult = mid_init_no_config();
		else
			initresult = mid_init(config.c_str());

		logit("Timidity: %s: %d", config.c_str(), initresult);

		// Is there a better way to signal failed init?
		// The decoder-init-function may not return errors AFAIK...
		if(initresult < 0)
		{
			if (config.empty() || config == "yes") config = "<default>";
			fatal("TiMidity-Plugin: Error processing TiMidity-Configuration!\n"
			"Configuration file is: %s", config.c_str());
		}

		midioptions.rate = TiMidity_Rate;
		midioptions.format = (TiMidity_Bits==16)?MID_AUDIO_S16LSB:MID_AUDIO_S8;
		midioptions.channels = TiMidity_Channels;
		midioptions.buffer_size = midioptions.rate;
	}
};

Decoder *timidity_plugin ()
{
	return new timidity_decoder;
}