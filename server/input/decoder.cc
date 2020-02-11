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
#include "decoder.h"
#include "io.h"
#include "inputs.h"
#include <taglib/fileref.h>
#include <taglib/tag.h>

//-----------------------------------------------------------------------------
// Decoder methods
//-----------------------------------------------------------------------------

void decoder_error::warn(const char *format, ...)
{
	type = ERROR_STREAM;
	va_list va; va_start(va, format);
	desc = format_va (format, va);
	va_end (va);
}
void decoder_error::fatal(const char *format, ...)
{
	type = ERROR_FATAL;
	va_list va; va_start (va, format);
	desc = format_va (format, va);
	va_end (va);
}

Codec* Decoder::open(const str &file)
{
	io_stream *stream = io_open(file.c_str(), 1);

	if (!io_ok (stream))
	{
		io_close(stream);
		return NULL;
	}

	return open(*stream);
}

void Decoder::read_tags(const str &file_name, file_tags &info)
{
	TagLib::FileRef f(file_name.c_str());

	if (!f.isNull() && f.tag())
	{
		TagLib::Tag *tag = f.tag();
		info.artist = tag->artist().to8Bit(true);
		info.album  = tag->album().to8Bit(true);
		info.title  = tag->title().to8Bit(true);
		info.track  = tag->track();
	}
	if (!f.isNull() && f.audioProperties())
	{
		TagLib::AudioProperties *properties = f.audioProperties();
		info.time = properties->length();
	}
	else
	{
		info.time = get_duration(file_name);
	}
}
bool Decoder::write_tags(const str &file_name, const tag_changes &info)
{
	TagLib::FileRef f(file_name.c_str());

	if (f.isNull() || !f.tag()) return false;

	TagLib::Tag *tag = f.tag();
	#define CVT(s) TagLib::String(*info.s, TagLib::String::UTF8)
	if (info.title ) tag->setTitle (CVT(title));
	if (info.artist) tag->setArtist(CVT(artist));
	if (info.album ) tag->setAlbum (CVT(album));
	if (info.track ) tag->setTrack (*info.track);
	#undef CVT

	return f.save();
}
bool Decoder::can_write_tags(const str &file_name)
{
	TagLib::FileRef f(file_name.c_str());

	if (f.isNull() || !f.tag()) return false;

	return true; // TODO, this is a bit optimistic
}

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

struct plugin
{
	const char *name;
	Decoder *decoder;
};
static std::vector<plugin> plugins;

static void split_mime(str &type, str &subtype)
{
	auto i = type.find('/');
	if (i == str::npos) return;
	
	subtype = type.substr(i+1);
	type = type.substr(0, i);

	if (has_prefix(subtype, "x-", true)) subtype = subtype.substr(2);
	i = subtype.find(';');
	if (i != str::npos) subtype = subtype.substr(0, i);
}

struct decoder_preference
{
	decoder_preference(const str &s)
	{
		strings tokens = split(s, "(,)");
		assert(tokens.size() >= 1);
		type = tokens[0]; tokens.erase(tokens.begin());
		split_mime(type, subtype);

		int asterisk_at = -1;
		std::set<int> done;

		/* Add the index of each known decoder to the decoders list.
		* Note the position following the first asterisk. */
		for (auto &name : tokens)
		{
			if (name == "*")
			{
				if (asterisk_at == -1) asterisk_at = decoder_list.size();
				continue;
			}

			int d = 0; for (; d < plugins.size(); ++d) if (!strcasecmp(plugins[d].name, name.c_str())) break;
			if (d >= plugins.size() || done.count(d)) continue;
			decoder_list.push_back(d);
			done.insert(d);
		}

		if (asterisk_at == -1) return;

		for (int i = 0; i < plugins.size(); ++i)
		{
			if (done.count(i)) continue;
			decoder_list.insert(decoder_list.begin() + asterisk_at++, i);
		}
	}
	std::vector<int> decoder_list; // plugin indices
	str type, subtype; // MIME type or extension
};
static std::vector<decoder_preference> preferences;
static std::vector<int> default_decoder_list;

/* Return the index of the first decoder able to handle audio with the
 * given filename extension and/or MIME media type, or -1 if none can. */
static Decoder* find_decoder (const char *file, str *mime)
{
	const std::vector<int> *decoder_list = NULL;

	// lookup by mime type first, if we have it
	str m, type, subtype;
	if (mime && !mime->empty())
	{
		type = *mime;
		split_mime(type, subtype);
	}
	else if (options::UseMimeMagic && file && *file)
	{
		m = type = file_mime_type(file);
		split_mime(type, subtype);
		mime = m.empty() ? NULL : &m;
	}
	else mime = NULL;
	if (!type.empty()) for (auto &pref : preferences)
	{
		if (strcasecmp(pref.type.c_str(), type.c_str()) ||
		    strcasecmp(pref.subtype.c_str(), subtype.c_str())) continue;
		decoder_list = &pref.decoder_list;
		break;
	}

	// if not found, try extension
	const char *ext = file ? ext_pos(file) : NULL;
	if (!decoder_list && ext) for (auto &pref : preferences)
	{
		if (!pref.subtype.empty() || strcasecmp(pref.type.c_str(), ext)) continue;
		decoder_list = &pref.decoder_list;
		break;
	}
	if (!mime && !ext) return NULL;

	// try all of them, if we have no preference
	if (!decoder_list) decoder_list = &default_decoder_list;

	// return first match
	// TODO: maybe this should return the first codec...
	// at least for get_decoder?
	for (int i : *decoder_list) {
		auto *d = plugins[i].decoder;
		if (mime && d->matches_mime(*mime)) return d;
		if (ext  && d->matches_ext (ext))   return d;
	}
	return NULL;
}

bool is_sound_file (const str &name)
{
	return find_decoder(name.c_str(), NULL);
}

struct Decoder *get_decoder (const str &file)
{
	return find_decoder(file.c_str(), NULL);
}

/* Return the decoder for this stream. */
Decoder *get_decoder_by_content (io_stream &stream)
{
	char buf[8096];
	ssize_t res;

	/* Peek at the start of the stream to check if sufficient data is
	 * available.  If not, there is no sense in trying the decoders as
	 * each of them would issue an error.  The data is also needed to
	 * get the MIME type. */
	logit ("Testing the stream...");
	res = io_peek (&stream, buf, sizeof (buf));
	if (res < 0) {
		error("Stream error: %s", io_strerror (&stream));
		return NULL;
	}

	if (res < 512) {
		logit ("Stream too short");
		return NULL;
	}

	// get decoder by mime type
	const char *tmp = io_get_mime_type (&stream);
	if (tmp) {
		str mime = tmp;
		Decoder *d = find_decoder (NULL, &mime);
		if (d) {
			logit ("Found decoder for MIME type %s", mime.c_str());
			return d;
		}
	}
	else logit ("No MIME type.");

	for (auto &p : plugins) {
		if (p.decoder->can_decode(stream)) {
			logit ("Found decoder for stream: %s", p.name);
			return p.decoder;
		}
	}

	error("Format not supported");
	return NULL;
}

void decoder_init()
{
	#define H(X) do{ auto *d = X ## _plugin(); if (d) plugins.push_back({#X, d}); }while(0)
	ALL_INPUTS
	#undef H

	for (int i = 0; i < plugins.size(); ++i)
		default_decoder_list.push_back(i);

	const char *PreferredDecoders[] = {
	"aac(aac,ffmpeg)", "m4a(ffmpeg)", "mpc(musepack,*,ffmpeg)", "mpc8(musepack,*,ffmpeg)",
	"sid(sidplay2)", "mus(sidplay2)", "wav(sndfile,*,ffmpeg)", "wv(wavpack,*,ffmpeg)",
	"audio/aac(aac)", "audio/aacp(aac)", "audio/m4a(ffmpeg)", "audio/wav(sndfile,*)",
	"oga(vorbis,*,ffmpeg)", "ogg(vorbis,*,ffmpeg)", "ogv(ffmpeg)", "application/ogg(vorbis)",
	"audio/ogg(vorbis)", "flac(flac,*,ffmpeg)", "opus(ffmpeg)", "spx(speex)", "noise(noise)"};
	for (auto *s : PreferredDecoders) preferences.emplace_back(s);
}

void decoder_cleanup ()
{
	preferences.clear();
	default_decoder_list.clear();
	for (auto &p : plugins) delete p.decoder;
}
