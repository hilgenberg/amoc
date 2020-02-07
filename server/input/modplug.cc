/*
* MOC - music on console
* Copyright (C) 2004 Damian Pietras <daper@daper.net>
*
* libmodplug-plugin Copyright (C) 2006 Hendrik Iben <hiben@tzi.de>
* Enables MOC to play modules via libmodplug (actually just a wrapper around
* libmodplug's C-wrapper... :-)).
*
* Based on ideas from G"urkan Seng"un's modplugplay. A command line
* interface to the modplugxmms library.
* Structure of this plugin is an adaption of the libsndfile-plugin from
* moc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
*/

#include <libmodplug/modplug.h>
#include "io.h"
#include "decoder.h"

// Limiting maximum size for loading a module was suggested by Damian.
// I've never seen such a large module so this should be a safe limit...
#ifndef MAXMODSIZE
#define MAXMODSIZE 1024*1024*42
#endif

struct modplug_data : public Codec
{
	ModPlugFile *modplugfile;
	int length;
	char *filedata;
	ModPlug_Settings settings;

	modplug_data(const char *file)
	{
		ModPlug_GetSettings(&settings);
		settings.mFlags = 0;
		settings.mFlags |= MODPLUG_ENABLE_OVERSAMPLING;
		settings.mFlags |= MODPLUG_ENABLE_NOISE_REDUCTION;
		//settings.mFlags |= MODPLUG_ENABLE_REVERB;
		//settings.mFlags |= MODPLUG_ENABLE_MEGABASS;
		//settings.mFlags |= MODPLUG_ENABLE_SURROUND;

		settings.mResamplingMode = MODPLUG_RESAMPLE_FIR;
		//settings.mResamplingMode = MODPLUG_RESAMPLE_SPLINE;
		//settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
		//settings.mResamplingMode = MODPLUG_RESAMPLE_NEAREST;
		settings.mChannels = 2;
		settings.mBits = 16;
		settings.mFrequency = 44100;
		settings.mReverbDepth = 0;
		settings.mReverbDelay = 0;
		settings.mBassAmount = 0;
		settings.mBassRange = 10;
		settings.mSurroundDepth = 0;
		settings.mSurroundDelay = 0;
		settings.mLoopCount = 0;
		ModPlug_SetSettings(&settings);

		modplugfile = NULL;
		filedata = NULL;

		struct io_stream *s = io_open(file, 0);
		if(!io_ok(s))
		{
			error.fatal("Can't open file: %s", file);
			io_close(s);
			return;
		}

		off_t size = io_file_size(s);

		if (!RANGE(1, size, INT_MAX)) {
			error.fatal("Module size unsuitable for loading: %s", file);
			io_close(s);
		}

		filedata = (char *)xmalloc((size_t)size);

		io_read(s, filedata, (size_t)size);
		io_close(s);

		modplugfile = ModPlug_Load(filedata, (int)size);

		if (!modplugfile) {
			free(filedata);
			error.fatal("Can't load module: %s", file);
			return;
		}

		length = ModPlug_GetLength(modplugfile);
	}

	~modplug_data()
	{
		if (modplugfile) {
			ModPlug_Unload(modplugfile);
			free(filedata);
		}
	}

	int seek (int sec) override
	{
		assert (sec >= 0);

		int ms = sec*1000;
		ms = MIN(ms,length);

		ModPlug_Seek(modplugfile, ms);

		return ms/1000;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params) override
	{
		sound_params.channels = settings.mChannels;
		sound_params.rate = settings.mFrequency;
		sound_params.fmt = ((settings.mBits==16)?SFMT_S16:(settings.mBits==8)?SFMT_S8:SFMT_S32) | SFMT_NE;

		return ModPlug_Read(modplugfile, buf, buf_len);
	}

	int get_duration () const override
	{
		return length/1000;
	}
};

struct modplug_decoder : public Decoder
{
	Codec* open(const str &file) override
	{
		return new modplug_data(file.c_str());
	}

	void read_tags(const str &file_name, file_tags &info) override
	{
		modplug_data data(file_name.c_str());
		if(!data.modplugfile) return;

		info.time = data.length / 1000;
		const char *tmp = ModPlug_GetName(data.modplugfile);
		if (tmp) info.title = tmp;
	}

	bool matches_ext(const char *ext) const override
	{
		// Do not include non-module formats in this list (even if
		// ModPlug supports them).  Doing so may cause memory exhaustion
		// in make_modplug_data().
		return
		!strcasecmp (ext, "NONE") ||
		!strcasecmp (ext, "MOD") ||
		!strcasecmp (ext, "S3M") ||
		!strcasecmp (ext, "XM") ||
		!strcasecmp (ext, "MED") ||
		!strcasecmp (ext, "MTM") ||
		!strcasecmp (ext, "IT") ||
		!strcasecmp (ext, "669") ||
		!strcasecmp (ext, "ULT") ||
		!strcasecmp (ext, "STM") ||
		!strcasecmp (ext, "FAR") ||
		!strcasecmp (ext, "AMF") ||
		!strcasecmp (ext, "AMS") ||
		!strcasecmp (ext, "DSM") ||
		!strcasecmp (ext, "MDL") ||
		!strcasecmp (ext, "OKT") ||
		// modplug can do MIDI but not in this form...
		//!strcasecmp (ext, "MID") ||
		!strcasecmp (ext, "DMF") ||
		!strcasecmp (ext, "PTM") ||
		!strcasecmp (ext, "DBM") ||
		!strcasecmp (ext, "MT2") ||
		!strcasecmp (ext, "AMF0") ||
		!strcasecmp (ext, "PSM") ||
		!strcasecmp (ext, "J2B") ||
		!strcasecmp (ext, "UMX");
	}
};


Decoder *mod_plugin ()
{
	return new modplug_decoder;
}
