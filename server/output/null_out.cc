/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* Fake output device - only for testing. */

#include "../audio.h"

struct null_driver : public AudioDriver
{
	sound_params params;

	null_driver(output_driver_caps &caps)
	{
		caps.formats = SFMT_S8 | SFMT_S16 | SFMT_LE;
		caps.min_channels = 1;
		caps.max_channels = 2;

		params = { 0, 0, 0 };
	}

	bool open (const sound_params &sound_params) override
	{
		params = sound_params;
		return true;
	}
	void close () override { params.rate = 0; }

	int play (const char *, size_t size) override
	{
		xsleep (size, audio_get_bps());
		return size;
	}
	bool reset () override { return true; }
	int get_buff_fill () const override { return 0; }

	int get_rate () const override { return params.rate; }

	int  read_mixer () const override { return 100; }
	void set_mixer(int) override { }
	void toggle_mixer_channel () override { }
	str  get_mixer_channel_name () const override { return "FakeMixer"; }
};

AudioDriver *NOSOUND_init(output_driver_caps &caps)
{
	return new null_driver(caps);
}
