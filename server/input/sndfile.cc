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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sndfile.h>

#include "decoder.h"
#include "../server.h"

/* TODO:
 * - sndfile is not thread-safe: use a mutex?
 * - some tags can be read.
 */

static std::set<str> supported_extns;

static void load_extn_list ()
{
	if (!supported_extns.empty()) return;
	const int counts[] = {SFC_GET_SIMPLE_FORMAT_COUNT,
	                      SFC_GET_FORMAT_MAJOR_COUNT};
	const int formats[] = {SFC_GET_SIMPLE_FORMAT,
	                       SFC_GET_FORMAT_MAJOR};

	for (size_t ix = 0; ix < ARRAY_SIZE(counts); ix += 1) {
		int limit;
		SF_FORMAT_INFO format_info;

		sf_command (NULL, counts[ix], &limit, sizeof (limit));
		for (int iy = 0 ; iy < limit ; iy += 1) {
			format_info.format = iy ;
			sf_command (NULL, formats[ix], &format_info, sizeof (format_info));
			supported_extns.insert(format_info.extension);
		}
	}

	/* These are synonyms of supported extensions. */
	#define SYN(x,y) if (supported_extns.count(x)) supported_extns.insert(y)
	SYN("aiff", "aif");
	SYN("au", "snd");
	SYN("wav", "nist");
	SYN("wav", "sph");
	SYN("iff", "svx");
	SYN("oga", "ogg");
	SYN("sf", "ircam");
	SYN("mat", "mat4");
	SYN("mat", "mat5");
	#undef SYN
}

struct sndfile_data : public Codec
{
	SNDFILE *sndfile;
	SF_INFO snd_info;

	/* Return true iff libsndfile's frame count is unknown or miscalculated. */
	bool is_timing_broken (int fd)
	{
		int rc;
		struct stat buf;
		SF_INFO *info = &snd_info;

		if (info->frames == SF_COUNT_MAX) return true;
		if (info->frames / info->samplerate > INT32_MAX) return true;

		/* The libsndfile code warns of miscalculation for huge files of
		* specific formats, but it's unclear if others are known to work
		* or the test is just omitted for them.  We'll assume they work
		* until it's shown otherwise. */
		switch (info->format & SF_FORMAT_TYPEMASK) {
		case SF_FORMAT_AIFF:
		case SF_FORMAT_AU:
		case SF_FORMAT_SVX:
		case SF_FORMAT_WAV:
			rc = fstat (fd, &buf);
			if (rc == -1) {
				log_errno ("Can't stat file", errno);
				/* We really need to return "unknown" here. */
				return false;
			}

			if (buf.st_size > UINT32_MAX)
				return true;
		}

		return false;
	}

	sndfile_data(const str &file)
	: sndfile(NULL)
	{
		memset (&snd_info, 0, sizeof(snd_info));

		int fd = open (file.c_str(), O_RDONLY);
		if (fd == -1) {
			char *err = xstrerror (errno);
			error.fatal("Can't open file: %s", err);
			free (err);
			return;
		}

		/* sf_open_fd() close()s 'fd' on error and in sf_close(). */
		sndfile = sf_open_fd (fd, SFM_READ, &snd_info, SF_TRUE);
		if (!sndfile) {
			/* FIXME: sf_strerror is not thread safe with NULL argument */
			error.fatal("Can't open file: %s", sf_strerror(NULL));
			return;
		}

		/* If the timing is broken, sndfile only decodes up to the broken value. */
		if (is_timing_broken (fd)) {
			error.fatal("File too large for audio format!");
			return;
		}

		debug ("Opened file %s", file.c_str());
		debug ("Channels: %d", snd_info.channels);
		debug ("Format: %08X", snd_info.format);
		debug ("Sample rate: %d", snd_info.samplerate);
	}

	~sndfile_data()
	{
		if (sndfile) sf_close (sndfile);
	}

	int seek (int sec) override
	{
		int res = sf_seek (sndfile, snd_info.samplerate * sec, SEEK_SET);
		if (res < 0) return -1;
		return res / snd_info.samplerate;
	}

	int decode (char *buf, int buf_len, sound_params &sound_params)
	{
		sound_params.channels = snd_info.channels;
		sound_params.rate = snd_info.samplerate;
		sound_params.fmt = SFMT_FLOAT;

		return sf_readf_float (sndfile, (float *)buf,
				buf_len / sizeof(float) / snd_info.channels)
			* sizeof(float) * snd_info.channels;
	}

	int get_duration () const override
	{
		return snd_info.frames / snd_info.samplerate;
	}
};

struct sndfile_decoder : public Decoder
{
	sndfile_decoder()
	{
		load_extn_list ();
	}

	~sndfile_decoder()
	{
		supported_extns.clear();
	}

	Codec* open(const str &f) { return new sndfile_data(f); }

	bool matches_ext(const char *ext) const override
	{
		str s(ext);
		std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c){ return c >= 'A' && c <= 'Z' ? c-('A'-'a') : c; });
		return supported_extns.count(s);
	}

	int get_duration(const str &file_name) override
	{
		sndfile_data data(file_name);
		return data.error.type == ERROR_OK ? data.get_duration() : -1;
	}
};

Decoder *sndfile_plugin ()
{
	return new sndfile_decoder;
}
