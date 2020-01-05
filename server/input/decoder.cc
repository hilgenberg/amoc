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
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>

#include "decoder.h"
#include "io.h"
#include "inputs.h"

static struct plugin {
	const char *name;
	struct decoder *decoder;
} plugins[16];

#define PLUGINS_NUM			(ARRAY_SIZE(plugins))

static int plugins_num = 0;

static bool have_tremor = false;

/* This structure holds the user's decoder preferences for audio formats. */
struct decoder_s_preference {
	struct decoder_s_preference *next;    /* chain pointer */
	int decoders;                         /* number of decoders */
	int decoder_list[PLUGINS_NUM];        /* decoder indices */
	char *subtype;                        /* MIME subtype or NULL */
	char type[];                          /* MIME type or filename extn */
};
typedef struct decoder_s_preference decoder_t_preference;
static decoder_t_preference *preferences = NULL;
static int default_decoder_list[PLUGINS_NUM];

static char *clean_mime_subtype (char *subtype)
{
	char *ptr;

	assert (subtype && subtype[0]);

	if (!strncasecmp (subtype, "x-", 2))
		subtype += 2;

	ptr = strchr (subtype, ';');
	if (ptr)
		*ptr = 0x00;

	return subtype;
}

/* Find a preference entry matching the given filename extension and/or
 * MIME media type, or NULL. */
static decoder_t_preference *lookup_preference (const char *extn,
                                                const char *file,
                                                char **mime)
{
	char *type, *subtype;
	decoder_t_preference *result;

	assert ((extn && extn[0]) || (file && file[0])
	                          || (mime && *mime && *mime[0]));

	type = NULL;
	subtype = NULL;
	for (result = preferences; result; result = result->next) {
		if (!result->subtype) {
			if (extn && !strcasecmp (result->type, extn))
				break;
		}
		else {

			if (!type) {
				if (mime && *mime == NULL && file && file[0]) {
					if (options::UseMimeMagic)
						*mime = file_mime_type (file);
				}
				if (mime && *mime && strchr (*mime, '/'))
					type = xstrdup (*mime);
				if (type) {
					subtype = strchr (type, '/');
					*subtype++ = 0x00;
					subtype = clean_mime_subtype (subtype);
				}
			}

			if (type) {
				if (!strcasecmp (result->type, type) &&
			    	!strcasecmp (result->subtype, subtype))
					break;
			}

		}
	}

	free (type);
	return result;
}

/* Return the index of the first decoder able to handle files with the
 * given filename extension, or -1 if none can. */
static int find_extn_decoder (int *decoder_list, int count, const char *extn)
{
	int ix;

	assert (decoder_list);
	assert (RANGE(0, count, plugins_num));
	assert (extn && extn[0]);

	for (ix = 0; ix < count; ix += 1) {
		if (plugins[decoder_list[ix]].decoder->our_format_ext &&
		    plugins[decoder_list[ix]].decoder->our_format_ext (extn))
			return decoder_list[ix];
	}

	return -1;
}

/* Return the index of the first decoder able to handle audio with the
 * given MIME media type, or -1 if none can. */
static int find_mime_decoder (int *decoder_list, int count, const char *mime)
{
	int ix;

	assert (decoder_list);
	assert (RANGE(0, count, plugins_num));
	assert (mime && mime[0]);

	for (ix = 0; ix < count; ix += 1) {
		if (plugins[decoder_list[ix]].decoder->our_format_mime &&
		    plugins[decoder_list[ix]].decoder->our_format_mime (mime))
			return decoder_list[ix];
	}

	return -1;
}

/* Return the index of the first decoder able to handle audio with the
 * given filename extension and/or MIME media type, or -1 if none can. */
static int find_decoder (const char *extn, const char *file, char **mime)
{
	int result;
	decoder_t_preference *pref;

	assert ((extn && extn[0]) || (file && file[0]) || (mime && *mime));

	pref = lookup_preference (extn, file, mime);
	if (pref) {
		if (pref->subtype)
			return find_mime_decoder (pref->decoder_list, pref->decoders, *mime);
		else
			return find_extn_decoder (pref->decoder_list, pref->decoders, extn);
	}

	result = -1;
	if (mime && *mime)
		result = find_mime_decoder (default_decoder_list, plugins_num, *mime);
	if (result == -1 && extn && *extn)
		result = find_extn_decoder (default_decoder_list, plugins_num, extn);

	return result;
}

/* Find the index in plugins table for the given file.
 * Return -1 if not found. */
static int find_type (const char *file)
{
	int result = -1;
	const char *extn;
	char *mime;

	extn = ext_pos (file);
	mime = NULL;

	result = find_decoder (extn, file, &mime);

	free (mime);
	return result;
}

int is_sound_file (const char *name)
{
	return find_type(name) != -1 ? 1 : 0;
}

/* Return short type name for the given file or NULL if not found.
 * Not thread safe! */
char *file_type_name (const char *file)
{
	int i;
	static char buf[4];

	if (is_url (file)) {
		strcpy (buf, "NET");
		return buf;
	}

	i = find_type (file);
	if (i == -1)
		return NULL;

	memset (buf, 0, sizeof (buf));
	if (plugins[i].decoder->get_name)
		plugins[i].decoder->get_name (file, buf);

	/* Attempt a default name if we have nothing else. */
	if (!buf[0]) {
		const char *ext;

		ext = ext_pos (file);
		if (ext) {
			size_t len;

			len = strlen (ext);
			switch (len) {
				default:
					buf[2] = toupper (ext[len - 1]);
				case 2:
					buf[1] = toupper (ext[1]);
				case 1:
					buf[0] = toupper (ext[0]);
				case 0:
					break;
			}
		}
	}

	if (!buf[0])
		return NULL;

	return buf;
}

struct decoder *get_decoder (const char *file)
{
	int i;

	i = find_type (file);
	if (i != -1)
		return plugins[i].decoder;

	return NULL;
}

/* Given a decoder pointer, return its name. */
const char *get_decoder_name (const struct decoder *decoder)
{
	int ix;
	const char *result = NULL;

	assert (decoder);

	for (ix = 0; ix < plugins_num; ix += 1) {
		if (plugins[ix].decoder == decoder) {
			result = plugins[ix].name;
			break;
		}
	}

	assert (result);

	return result;
}

/* Use the stream's MIME type to return a decoder for it, or NULL if no
 * applicable decoder was found. */
static struct decoder *get_decoder_by_mime_type (struct io_stream *stream)
{
	int i;
	char *mime;
	struct decoder *result;

	result = NULL;
	mime = xstrdup (io_get_mime_type (stream));
	if (mime) {
		i = find_decoder (NULL, NULL, &mime);
		if (i != -1) {
			logit ("Found decoder for MIME type %s: %s", mime, plugins[i].name);
			result = plugins[i].decoder;
		}
		free (mime);
	}
	else
		logit ("No MIME type.");

	return result;
}

/* Return the decoder for this stream. */
struct decoder *get_decoder_by_content (struct io_stream *stream)
{
	char buf[8096];
	ssize_t res;
	int i;
	struct decoder *decoder_by_mime_type;

	assert (stream != NULL);

	/* Peek at the start of the stream to check if sufficient data is
	 * available.  If not, there is no sense in trying the decoders as
	 * each of them would issue an error.  The data is also needed to
	 * get the MIME type. */
	logit ("Testing the stream...");
	res = io_peek (stream, buf, sizeof (buf));
	if (res < 0) {
		error("Stream error: %s", io_strerror (stream));
		return NULL;
	}

	if (res < 512) {
		logit ("Stream too short");
		return NULL;
	}

	decoder_by_mime_type = get_decoder_by_mime_type (stream);
	if (decoder_by_mime_type)
		return decoder_by_mime_type;

	for (i = 0; i < plugins_num; i++) {
		if (plugins[i].decoder->can_decode
				&& plugins[i].decoder->can_decode (stream)) {
			logit ("Found decoder for stream: %s", plugins[i].name);
			return plugins[i].decoder;
		}
	}

	error("Format not supported");
	return NULL;
}

/* Return the index for a decoder of the given name, or plugins_num if
 * not found. */
static int lookup_decoder_by_name (const char *name)
{
	int result;

	assert (name && name[0]);

	result = 0;
	while (result < plugins_num) {
		if (!strcasecmp (plugins[result].name, name))
			break;
		result += 1;
	}

	return result;
}

/* Return a string of concatenated driver names. */
static std::string list_decoder_names (int *decoder_list, int count)
{
	std::string result;

	for (int ix = 0; ix < count; ix += 1)
	{
		if (ix > 0) result += " ";
		std::string s = plugins[decoder_list[ix]].name;
		if (have_tremor && s == "vorbis") s = "vorbis(tremor)";
		#if !defined(HAVE_FFMPEG)
		else if (s == "ffmpeg")
			#if defined(HAVE_LIBAV)
			s = "ffmpeg(libav)";
			#else
			s = "ffmpeg/libav";
			#endif
		#endif
		result += s;
	}

	return result;
}

/* Create a new preferences entry and initialise it. */
static decoder_t_preference *make_preference (const std::string &prefix)
{
	decoder_t_preference *result;

	result = (decoder_t_preference *)xmalloc (
		offsetof (decoder_t_preference, type) + prefix.length() + 1);
	result->next = NULL;
	result->decoders = 0;
	strcpy (result->type, prefix.c_str());
	result->subtype = strchr (result->type, '/');
	if (result->subtype) {
		*result->subtype++ = 0x00;
		result->subtype = clean_mime_subtype (result->subtype);
	}

	return result;
}

/* Is the given decoder (by index) already in the decoder list for 'pref'? */
static bool is_listed_decoder (decoder_t_preference *pref, int d)
{
	int ix;
	bool result;

	assert (pref);
	assert (d >= 0);

	result = false;
	for (ix = 0; ix < pref->decoders; ix += 1) {
		if (d == pref->decoder_list[ix]) {
			result = true;
			break;
		}
	}

	return result;
}

/* Add the named decoder (if valid) to a preferences decoder list. */
static void load_each_decoder (decoder_t_preference *pref, const std::string &name)
{
	int d;

	assert (pref);

	d = lookup_decoder_by_name (name.c_str());

	/* Drop unknown decoders. */
	if (d == plugins_num)
		return;

	/* Drop duplicate decoders. */
	if (is_listed_decoder (pref, d))
		return;

	pref->decoder_list[pref->decoders++] = d;

	return;
}

/* Build a preference's decoder list. */
static void load_decoders (decoder_t_preference *pref, const stringlist &tokens)
{
	int ix, dx, asterisk_at;
	int decoder[PLUGINS_NUM];

	assert (pref);

	asterisk_at = -1;

	/* Add the index of each known decoder to the decoders list.
	 * Note the position following the first asterisk. */
	for (auto &name : tokens) {
		if (name == "*")
			load_each_decoder (pref, name);
		else if (asterisk_at == -1)
			asterisk_at = pref->decoders;
	}

	if (asterisk_at == -1)
		return;

	dx = 0;

	/* Find decoders not already listed. */
	for (ix = 0; ix < plugins_num; ix += 1) {
		if (!is_listed_decoder (pref, ix))
			decoder[dx++] = ix;
	}

	/* Splice asterisk decoders into the decoder list. */
	for (ix = 0; ix < dx; ix += 1) {
		pref->decoder_list[pref->decoders++] =
		      pref->decoder_list[asterisk_at + ix];
		pref->decoder_list[asterisk_at + ix] = decoder[ix];
	}

	assert (RANGE(0, pref->decoders, plugins_num));
}

/* Load all preferences given by the user in PreferredDecoders. */
static void load_preferences ()
{
	const char *PreferredDecoders[] = {
	"aac(aac,ffmpeg)", "m4a(ffmpeg)", "mpc(musepack,*,ffmpeg)", "mpc8(musepack,*,ffmpeg)",
	"sid(sidplay2)", "mus(sidplay2)", "wav(sndfile,*,ffmpeg)", "wv(wavpack,*,ffmpeg)",
	"audio/aac(aac)", "audio/aacp(aac)", "audio/m4a(ffmpeg)", "audio/wav(sndfile,*)",
	"oga(vorbis,*,ffmpeg)", "ogg(vorbis,*,ffmpeg)", "ogv(ffmpeg)", "application/ogg(vorbis)",
	"audio/ogg(vorbis)", "flac(flac,*,ffmpeg)", "opus(ffmpeg)", "spx(speex)"};

	for (auto *s : PreferredDecoders)
	{
		stringlist tokens = split(s, "(,)");
		assert(tokens.size() >= 1);
		decoder_t_preference *pref = make_preference (tokens[0]);
		load_decoders (pref, tokens);
		pref->next = preferences;
		preferences = pref;
	}
}

static void load_plugins (int debug_info)
{
	int ix;

#define H(X) \
	assert(plugins_num < PLUGINS_NUM);\
	plugins[plugins_num].decoder = X ## _plugin();\
	plugins[plugins_num].name = #X;\
	if (plugins[plugins_num].decoder->init) plugins[plugins_num].decoder->init();\
	debug("Loaded %s decoder", plugins[plugins_num].name);\
	++plugins_num

	ALL_INPUTS
#undef H

	/* Is the Vorbis decoder using Tremor? */
	have_tremor = vorbis_has_tremor();

	for (ix = 0; ix < plugins_num; ix += 1)
		default_decoder_list[ix] = ix;

	std::string names = list_decoder_names (default_decoder_list, plugins_num);
	logit ("Loaded %d decoders:%s", plugins_num, names.c_str());
}

void decoder_init (int debug_info)
{
	load_plugins (debug_info);
	load_preferences ();
}

static void cleanup_decoders ()
{
	int ix;

	for (ix = 0; ix < plugins_num; ix++) {
		if (plugins[ix].decoder->destroy)
			plugins[ix].decoder->destroy ();
	}
}

static void cleanup_preferences ()
{
	decoder_t_preference *pref, *next;

	pref = preferences;
	for (pref = preferences; pref; pref = next) {
		next = pref->next;
		free (pref);
	}

	preferences = NULL;
}

void decoder_cleanup ()
{
	cleanup_decoders ();
	cleanup_preferences ();
}

/* Fill the error structure with an error of a given type and message.
 * strerror(add_errno) is appended at the end of the message if add_errno != 0.
 * The old error message is free()ed.
 * This is thread safe; use this instead of constructs using strerror(). */
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...)
{
	char *err_str;
	va_list va;

	if (error->err)
		free (error->err);

	error->type = type;

	va_start (va, format);
	err_str = format_msg_va (format, va);
	va_end (va);

	if (add_errno) {
		char *err_buf;

		err_buf = xstrerror (add_errno);
		error->err = format_msg ("%s%s", err_str, err_buf);
		free (err_buf);
	}
	else
		error->err = format_msg ("%s", err_str);

	free (err_str);
}

/* Initialize the decoder_error structure. */
void decoder_error_init (struct decoder_error *error)
{
	error->type = ERROR_OK;
	error->err = NULL;
}

/* Set the decoder_error structure to contain "success" information. */
void decoder_error_clear (struct decoder_error *error)
{
	error->type = ERROR_OK;
	if (error->err) {
		free (error->err);
		error->err = NULL;
	}
}

void decoder_error_copy (struct decoder_error *dst,
		const struct decoder_error *src)
{
	dst->type = src->type;
	dst->err = xstrdup (src->err);
}

/* Return the error text from the decoder_error variable. */
const char *decoder_error_text (const struct decoder_error *error)
{
	return error->err;
}
