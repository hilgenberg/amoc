/*
 * MOC - music on console
 * Copyright (C) 2004-2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Contributors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - "previous" request
 *
 */

#include <pthread.h>

#include "server.h"
#include "input/decoder.h"
#include "server_plist.h"
#include "../Socket.h"

#ifdef HAVE_OSS
# include "output/oss.h"
#endif
#ifdef HAVE_SNDIO
# include "output/sndio_out.h"
#endif
#ifdef HAVE_ALSA
# include "output/alsa.h"
#endif
#ifndef NDEBUG
# include "output/null_out.h"
#endif
#ifdef HAVE_JACK
# include "output/jack.h"
#endif

#include "output/softmixer.h"
#include "output/equalizer.h"

#include "output/out_buf.h"
#include "output/player.h"
#include "audio.h"
#include "input/io.h"
#include "output/audio_conversion.h"

static pthread_t playing_thread = 0;  /* tid of play thread */
static int play_thread_running = 0;

/* currently played file and Playlists. */
ServerPlaylist playlist;
static pthread_mutex_t plist_mtx = PTHREAD_MUTEX_INITIALIZER;


static struct out_buf *out_buf;
static struct hw_funcs hw;
static struct output_driver_caps hw_caps; /* capabilities of the output
					     driver */

/* Player state. */
static int state = STATE_STOP;
static int prev_state = STATE_STOP;

/* requests for playing thread */
static int stop_playing = 0;
static int play_next = 0;
static int play_prev = 0;
static pthread_mutex_t request_mtx = PTHREAD_MUTEX_INITIALIZER;


/* Is the audio device opened? */
static int audio_opened = 0;

/* Current sound parameters (with which the device is opened). */
static struct sound_params driver_sound_params = { 0, 0, 0 };

/* Sound parameters requested by the decoder. */
static struct sound_params req_sound_params = { 0, 0, 0 };

static struct audio_conversion sound_conv;
static int need_audio_conversion = 0;

/* URL of the last played stream. Used to fake pause/unpause of internet
 * streams. Protected by plist_mtx. */
static char *last_stream_url = NULL;

static int current_mixer = 0;

/* Check if the two sample rates don't differ so much that we can't play. */
#define sample_rate_compat(sound, device) ((device) * 1.05 >= sound \
		&& (device) * 0.95 <= sound)

/* Make a human readable description of the sound sample format(s).
 * Put the description in msg which is of size buf_size.
 * Return msg. */
char *sfmt_str (const long format, char *msg, const size_t buf_size)
{
	assert (sound_format_ok(format));

	assert (buf_size > 0);
	msg[0] = 0;

	if (format & SFMT_S8)
		strncat (msg, ", 8-bit signed", buf_size - strlen(msg) - 1);
	if (format & SFMT_U8)
		strncat (msg, ", 8-bit unsigned", buf_size - strlen(msg) - 1);
	if (format & SFMT_S16)
		strncat (msg, ", 16-bit signed", buf_size - strlen(msg) - 1);
	if (format & SFMT_U16)
		strncat (msg, ", 16-bit unsigned", buf_size - strlen(msg) - 1);
	if (format & SFMT_S32)
		strncat (msg, ", 24-bit signed (as 32-bit samples)",
				buf_size - strlen(msg) - 1);
	if (format & SFMT_U32)
		strncat (msg, ", 24-bit unsigned (as 32-bit samples)",
				buf_size - strlen(msg) - 1);
	if (format & SFMT_FLOAT)
		strncat (msg, ", float",
				buf_size - strlen(msg) - 1);

	if (format & SFMT_LE)
		strncat (msg, " little-endian", buf_size - strlen(msg) - 1);
	else if (format & SFMT_BE)
		strncat (msg, " big-endian", buf_size - strlen(msg) - 1);
	if (format & SFMT_NE)
		strncat (msg, " (native)", buf_size - strlen(msg) - 1);

	/* skip first ", " */
	if (msg[0])
		memmove (msg, msg + 2, strlen(msg) + 1);

	return msg;
}

/* Return != 0 if fmt1 and fmt2 have the same sample width. */
int sfmt_same_bps (const long fmt1, const long fmt2)
{
	if (fmt1 & (SFMT_S8 | SFMT_U8)
			&& fmt2 & (SFMT_S8 | SFMT_U8))
		return 1;
	if (fmt1 & (SFMT_S16 | SFMT_U16)
			&& fmt2 & (SFMT_S16 | SFMT_U16))
		return 1;
	if (fmt1 & (SFMT_S32 | SFMT_U32)
			&& fmt2 & (SFMT_S32 | SFMT_U32))
		return 1;
	if (fmt1 & fmt2 & SFMT_FLOAT)
		return 1;

	return 0;
}

/* Return the best matching sample format for the requested format and
 * available format mask. */
static long sfmt_best_matching (const long formats_with_endian,
		const long req_with_endian)
{
	long formats = formats_with_endian & SFMT_MASK_FORMAT;
	long req = req_with_endian & SFMT_MASK_FORMAT;
	long best = 0;

	char fmt_name1[SFMT_STR_MAX];
	char fmt_name2[SFMT_STR_MAX];

	if (formats & req)
		best = req;
	else if (req == SFMT_S8 || req == SFMT_U8) {
		if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
		else if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
	}
	else if (req == SFMT_S16 || req == SFMT_U16) {
		if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
		else if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
	}
	else if (req == SFMT_S32 || req == SFMT_U32 || req == SFMT_FLOAT) {
		if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
		else if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
	}

	assert (best != 0);

	if (!(best & (SFMT_S8 | SFMT_U8))) {
		if ((formats_with_endian & SFMT_LE)
				&& (formats_with_endian & SFMT_BE))
			best |= SFMT_NE;
		else
			best |= formats_with_endian & SFMT_MASK_ENDIANNESS;
	}

	debug ("Chose %s as the best matching %s",
			sfmt_str(best, fmt_name1, sizeof(fmt_name1)),
			sfmt_str(req_with_endian, fmt_name2, sizeof(fmt_name2)));

	return best;
}

/* Return the number of bytes per sample for the given format. */
int sfmt_Bps (const long format)
{
	int Bps = -1;

	switch (format & SFMT_MASK_FORMAT) {
		case SFMT_S8:
		case SFMT_U8:
			Bps = 1;
			break;
		case SFMT_S16:
		case SFMT_U16:
			Bps = 2;
			break;
		case SFMT_S32:
		case SFMT_U32:
			Bps = 4;
			break;
		case SFMT_FLOAT:
			Bps = sizeof (float);
			break;
	}

	assert (Bps > 0);

	return Bps;
}

void audio_fail_file (const str &path)
{
	LOCK (plist_mtx);
	playlist.invalidate(path);
	UNLOCK (plist_mtx);
}

static void *play_thread (void *unused)
{
	logit ("Entering playing thread");

	while (true) {

		LOCK (plist_mtx);
		if (playlist.stopped()) { UNLOCK(plist_mtx); break; }
		str file = playlist.path(playlist.current());
		str next = playlist.path(playlist.next());
		UNLOCK (plist_mtx);

		play_next = 0;
		play_prev = 0;

		if (!file.empty()) {
			logit ("Playing %s", file.c_str());
			out_buf_time_set (out_buf, 0.0);

			const char *next_file = next.empty() ? NULL : next.c_str();
			player (file.c_str(), next_file, out_buf);

			set_info_rate (0);
			set_info_bitrate (0);
			set_info_channels (1);
			out_buf_time_set (out_buf, 0.0);
		}

		LOCK (plist_mtx);

		if (last_stream_url) {
			free (last_stream_url);
			last_stream_url = NULL;
		}

		if (stop_playing) {
			playlist.stop();
			logit ("stopped");
		}
		else if (play_prev) {
			logit ("Playing previous...");
			playlist.play(playlist.prev());
		}
		else if (play_next) {
			logit ("Playing next...");
			playlist.play(playlist.next(true));
		}
		else {
			logit ("Playing next in list...");
			playlist.play(playlist.next());
		}
		bool stopped = playlist.stopped();

		UNLOCK (plist_mtx);

		if (stopped) break;
	}

	prev_state = state;
	state = STATE_STOP;
	state_change ();

	audio_close ();
	logit ("Exiting");

	return NULL;
}

void audio_reset ()
{
	if (hw.reset)
		hw.reset ();
}

void audio_stop ()
{
	int rc;

	if (play_thread_running) {
		logit ("audio_stop()");
		LOCK (request_mtx);
		stop_playing = 1;
		UNLOCK (request_mtx);
		player_stop ();
		logit ("pthread_join (playing_thread, NULL)");
		rc = pthread_join (playing_thread, NULL);
		if (rc != 0)
			log_errno ("pthread_join() failed", rc);
		playing_thread = 0;
		play_thread_running = 0;
		stop_playing = 0;
		logit ("done stopping");
	}
	else if (state == STATE_PAUSE) {
		LOCK (plist_mtx);
		playlist.stop();
		UNLOCK (plist_mtx);

		prev_state = state;
		state = STATE_STOP;
		state_change ();
	}
}

/* Start playing from the file fname. If fname is an empty string,
 * start playing from the first file on the list. */
void audio_play (const str &fname)
{
	audio_stop ();
	player_reset ();

	LOCK (plist_mtx);
	if (fname.empty())
	{
		if (playlist.stopped())
			playlist.play(playlist.next(true));
	}
	else
	{
		playlist.play(fname);
	}
	UNLOCK (plist_mtx);

	int rc = pthread_create (&playing_thread, NULL, play_thread, NULL);
	if (rc != 0) error_errno ("Can't create thread", rc);
	play_thread_running = 1;
}
void audio_play (int idx)
{
	audio_stop ();
	player_reset ();

	LOCK (plist_mtx);
	playlist.play(idx);
	UNLOCK (plist_mtx);

	int rc = pthread_create (&playing_thread, NULL, play_thread, NULL);
	if (rc != 0) error_errno ("Can't create thread", rc);
	play_thread_running = 1;
}

void audio_next ()
{
	if (play_thread_running) {
		play_next = 1;
		player_stop ();
	}
}

void audio_prev ()
{
	if (play_thread_running) {
		play_prev = 1;
		player_stop ();
	}
}

void audio_pause ()
{
	LOCK (plist_mtx);

	auto *song = playlist.current_item();
	if (song) {
		if (song->type == F_URL) {
			UNLOCK (plist_mtx);
			audio_stop ();
			LOCK (plist_mtx);

			if (last_stream_url)
				free (last_stream_url);
			last_stream_url = xstrdup (song->path.c_str());
		}
		else
			out_buf_pause (out_buf);

		prev_state = state;
		state = STATE_PAUSE;
		state_change ();
	}

	UNLOCK (plist_mtx);
}

void audio_unpause ()
{
	LOCK (plist_mtx);
	if (last_stream_url && is_url(last_stream_url)) {
		char *url = xstrdup (last_stream_url);

		UNLOCK (plist_mtx);
		audio_play (url);
		free (url);
	}
	else if (!playlist.stopped()) {
		out_buf_unpause (out_buf);
		prev_state = state;
		state = STATE_PLAY;
		UNLOCK (plist_mtx);
		state_change ();
	}
	else
		UNLOCK (plist_mtx);
}

static void reset_sound_params (struct sound_params *params)
{
	params->rate = 0;
	params->channels = 0;
	params->fmt = 0;
}

/* Return 0 on error. If sound params == NULL, open the device using
 * the previous parameters. */
int audio_open (struct sound_params *sound_params)
{
	int res;
	static struct sound_params last_params = { 0, 0, 0 };

	if (!sound_params)
		sound_params = &last_params;
	else
		last_params = *sound_params;

	assert (sound_format_ok(sound_params->fmt));

	if (audio_opened) {
		if (sound_params_eq(req_sound_params, *sound_params)) {
			if (audio_get_bps() >= 88200) {
				logit ("Audio device already opened with such parameters.");
				return 1;
			}

			/* Not closing the device would cause that much
			 * sound from the previous file to stay in the buffer
			 * and the user will hear old data, so close it. */
			logit ("Reopening device due to low bps.");
		}

		audio_close ();
	}

	req_sound_params = *sound_params;

	/* Set driver_sound_params to parameters supported by the driver that
	 * are nearly the requested parameters. */

	if (options::ForceSampleRate) {
		driver_sound_params.rate = options::ForceSampleRate;
		logit ("Setting forced driver sample rate to %dHz",
				driver_sound_params.rate);
	}
	else
		driver_sound_params.rate = req_sound_params.rate;

	driver_sound_params.fmt = sfmt_best_matching (hw_caps.formats,
			req_sound_params.fmt);

	/* number of channels */
	driver_sound_params.channels = CLAMP(hw_caps.min_channels,
	                                     req_sound_params.channels,
	                                     hw_caps.max_channels);

	res = hw.open (&driver_sound_params);

	if (res) {
		char fmt_name[SFMT_STR_MAX];

		driver_sound_params.rate = hw.get_rate ();
		if (driver_sound_params.fmt != req_sound_params.fmt
				|| driver_sound_params.channels
				!= req_sound_params.channels
				|| (!sample_rate_compat(
						req_sound_params.rate,
						driver_sound_params.rate))) {
			logit ("Conversion of the sound is needed.");
			if (!audio_conv_new (&sound_conv, &req_sound_params,
					&driver_sound_params)) {
				hw.close ();
				reset_sound_params (&req_sound_params);
				return 0;
			}
			need_audio_conversion = 1;
		}
		audio_opened = 1;

		logit ("Requested sound parameters: %s, %d channels, %dHz",
				sfmt_str(req_sound_params.fmt, fmt_name, sizeof(fmt_name)),
				req_sound_params.channels,
				req_sound_params.rate);
		logit ("Driver sound parameters: %s, %d channels, %dHz",
				sfmt_str(driver_sound_params.fmt, fmt_name, sizeof(fmt_name)),
				driver_sound_params.channels,
				driver_sound_params.rate);
	}

	return res;
}

int audio_send_buf (const char *buf, const size_t size)
{
	size_t out_data_len = size;
	int res;
	char *converted = NULL;

	if (need_audio_conversion)
		converted = audio_conv (&sound_conv, buf, size, &out_data_len);

	if (need_audio_conversion && converted)
		res = out_buf_put (out_buf, converted, out_data_len);
	else if (!need_audio_conversion)
		res = out_buf_put (out_buf, buf, size);
	else
		res = 0;

	if (converted)
		free (converted);

	return res;
}

/* Get the current audio format bytes per frame value.
 * May return 0 if the audio device is closed. */
int audio_get_bpf ()
{
	return driver_sound_params.channels
		* (driver_sound_params.fmt ? sfmt_Bps(driver_sound_params.fmt)
				: 0);
}

/* Get the current audio format bytes per second value.
 * May return 0 if the audio device is closed. */
int audio_get_bps ()
{
	return driver_sound_params.rate * audio_get_bpf ();
}

int audio_get_buf_fill ()
{
	return hw.get_buff_fill ();
}

int audio_send_pcm (const char *buf, const size_t size)
{
	char *softmixed = NULL;
	char *equalized = NULL;

	if (equalizer_is_active ())
	{
		equalized = (char*) xmalloc (size);
		memcpy (equalized, buf, size);

		equalizer_process_buffer (equalized, size, &driver_sound_params);

		buf = equalized;
	}

	if (softmixer_is_active () || softmixer_is_mono ())
	{
		if (equalized)
		{
			softmixed = equalized;
		}
		else
		{
			softmixed = (char*) xmalloc (size);
			memcpy (softmixed, buf, size);
		}

		softmixer_process_buffer (softmixed, size, &driver_sound_params);

		buf = softmixed;
	}

	int played;

	played = hw.play (buf, size);

	if (played < 0)
		fatal ("Audio output error!");

	if (softmixed && !equalized)
		free (softmixed);

	if (equalized)
		free (equalized);

	return played;
}

/* Get current time of the song in seconds. */
int audio_get_time ()
{
	return state != STATE_STOP ? out_buf_time_get (out_buf) : 0;
}

void audio_close ()
{
	if (audio_opened) {
		reset_sound_params (&req_sound_params);
		reset_sound_params (&driver_sound_params);
		hw.close ();
		if (need_audio_conversion) {
			audio_conv_destroy (&sound_conv);
			need_audio_conversion = 0;
		}
		audio_opened = 0;
	}
}

/* Try to initialize drivers from the list and fill funcs with
 * those of the first working driver. */
static void find_working_driver (struct hw_funcs *funcs)
{
	//SoundDriver_t SoundDriver = SoundDriver_t::AUTO;
	//OPENBSD: "SNDIO:JACK:OSS", else "Jack:ALSA:OSS"
	using options::SoundDriver_t;
	auto d = options::SoundDriver;

	memset (funcs, 0, sizeof(*funcs));

	bool a = (d == SoundDriver_t::AUTO);
#ifdef HAVE_JACK
	if (a || d == SoundDriver_t::JACK) {
		memset (funcs, 0, sizeof(*funcs));
		moc_jack_funcs (funcs);
		if (a) printf ("Trying JACK...\n");
		if (funcs->init(&hw_caps)) return;
	}
#endif

#ifdef HAVE_ALSA
	if (a || d == SoundDriver_t::ALSA) {
		memset (funcs, 0, sizeof(*funcs));
		alsa_funcs (funcs);
		if (a) printf ("Trying ALSA...\n");
		if (funcs->init(&hw_caps)) return;
	}
#endif

#ifdef HAVE_OSS
	if (a || d == SoundDriver_t::OSS) {
		memset (funcs, 0, sizeof(*funcs));
		oss_funcs (funcs);
		if (a) printf ("Trying OSS...\n");
		if (funcs->init(&hw_caps)) return;
	}
#endif

#ifdef HAVE_SNDIO
	if (a || d == SoundDriver_t::SNDIO) {
		memset (funcs, 0, sizeof(*funcs));
		sndio_funcs (funcs);
		if (a) printf ("Trying SNDIO...\n");
		if (funcs->init(&hw_caps)) return;
	}
#endif

#ifndef NDEBUG
	if (d == SoundDriver_t::NOSOUND) {
		memset (funcs, 0, sizeof(*funcs));
		null_funcs (funcs);
		if (funcs->init(&hw_caps)) return;
	}
#endif

	fatal ("No valid sound driver!");
}

static void print_output_capabilities
            (const struct output_driver_caps *caps)
{
	char fmt_name[SFMT_STR_MAX];

	logit ("Sound driver capabilities: channels %d - %d, formats: %s",
			caps->min_channels, caps->max_channels,
			sfmt_str(caps->formats, fmt_name, sizeof(fmt_name)));
}

void audio_initialize ()
{
	find_working_driver (&hw);

	if (hw_caps.max_channels < hw_caps.min_channels)
		fatal ("Error initializing audio device: "
		       "device reports incorrect number of channels.");
	if (!sound_format_ok (hw_caps.formats))
		fatal ("Error initializing audio device: "
		       "device reports no usable formats.");

	print_output_capabilities (&hw_caps);
	if (!options::Allow24bitOutput
			&& hw_caps.formats & (SFMT_S32 | SFMT_U32)) {
		logit ("Disabling 24bit modes because Allow24bitOutput is set to no.");
		hw_caps.formats &= ~(SFMT_S32 | SFMT_U32);
		if (!sound_format_ok (hw_caps.formats))
			fatal ("No available sound formats after disabling 24bit modes. "
			       "Consider setting Allow24bitOutput to yes.");
	}

	out_buf = out_buf_new (options::OutputBuffer * 1024);

	softmixer_init();
	equalizer_init();

	player_init ();
}

void audio_exit ()
{
	int rc;

	audio_stop ();
	if (hw.shutdown)
		hw.shutdown ();
	out_buf_free (out_buf);
	out_buf = NULL;
	player_cleanup ();
	rc = pthread_mutex_destroy (&plist_mtx);
	if (rc != 0)
		log_errno ("Can't destroy plist_mtx", rc);
	rc = pthread_mutex_destroy (&request_mtx);
	if (rc != 0)
		log_errno ("Can't destroy request_mtx", rc);

	if (last_stream_url)
		free (last_stream_url);

	softmixer_shutdown();
	equalizer_shutdown();
}

void audio_seek (const int sec)
{
	LOCK (plist_mtx);
	bool playing = !playlist.stopped();
	UNLOCK (plist_mtx);

	if (playing && state == STATE_PLAY)
		player_seek (sec);
	else
		logit ("Seeking when nothing is played.");
}

void audio_jump_to (const int sec)
{
	LOCK (plist_mtx);
	bool playing = !playlist.stopped();
	UNLOCK (plist_mtx);

	if (playing && state == STATE_PLAY)
		player_jump_to (sec);
	else
		logit ("Jumping when nothing is played.");
}

int audio_get_state ()
{
	return state;
}

int audio_get_prev_state ()
{
	return prev_state;
}

void audio_plist_add (const str &file)
{
	LOCK (plist_mtx);
	playlist.add(file);
	UNLOCK (plist_mtx);
}

void audio_plist_add (const plist &pl, int idx)
{
	LOCK (plist_mtx);
	playlist.add(pl, idx);
	UNLOCK (plist_mtx);
}

void audio_plist_clear ()
{
	LOCK (plist_mtx);
	playlist.clear();
	UNLOCK (plist_mtx);
}

/* Returned memory is malloc()ed. */
void audio_get_current(str &path, int &idx)
{
	LOCK (plist_mtx);
	auto s = playlist.current();
	idx = s.first ? -1 : s.second; // return -1 for dir_plist songs
	path = playlist.path(s);
	UNLOCK (plist_mtx);
}

int audio_get_mixer ()
{
	if (current_mixer == 2)
		return softmixer_get_value ();

	return hw.read_mixer ();
}

void audio_set_mixer (const int val)
{
	if (!RANGE(0, val, 100)) {
		logit ("Tried to set mixer to volume out of range.");
		return;
	}

	if (current_mixer == 2)
		softmixer_set_value (val);
	else
		hw.set_mixer (val);
}

void audio_plist_delete (int i, int n)
{
	LOCK (plist_mtx);
	playlist.remove(i, n);
	UNLOCK (plist_mtx);
}

void audio_files_rm(const std::set<str> &files)
{
	LOCK (plist_mtx);
	playlist.remove(files);
	UNLOCK (plist_mtx);
}
void audio_files_mv(const std::set<str> &files, const str &dst)
{
	LOCK (plist_mtx);
	playlist.move(files, dst);
	UNLOCK (plist_mtx);
}
void audio_files_mv(const str &file, const str &new_path)
{
	LOCK (plist_mtx);
	playlist.rename(file, new_path);
	UNLOCK (plist_mtx);
}

void audio_send_plist(Socket &socket)
{
	LOCK (plist_mtx);
	try{
		socket.send(playlist.list());
	}
	catch (...)
	{
		UNLOCK (plist_mtx);
		throw;
	}
	UNLOCK (plist_mtx);
}

void audio_plist_set_and_play (plist &&pl, int idx)
{
	LOCK (plist_mtx);
	playlist.play(std::move(pl), idx);
	bool ok = !playlist.stopped();
	UNLOCK (plist_mtx);
	
	if (ok) audio_play(idx);
}

void audio_get_plist(plist &pl)
{
	LOCK (plist_mtx);

	pl.clear();
	pl += playlist.list();

	UNLOCK (plist_mtx);
}


/* Set the time for a file on the playlist. */
void audio_plist_set_time (const char *file, const int time)
{
	// TODO
	/*int i;

	LOCK (plist_mtx);
	if ((i = playlist.find(file)) != -1) {
		plist_set_item_time (&playlist, i, time);
		playlist.items[i].mtime = get_mtime (file);
		debug ("Setting time for %s", file);
	}
	else
		logit ("Request for updating time for a file not present on the"
				" playlist!");
	UNLOCK (plist_mtx);*/
}

/* Notify that the state was changed (used by the player). */
void audio_state_started_playing ()
{
	prev_state = state;
	state = STATE_PLAY;
	state_change ();
}

/* Swap 2 files on the playlist. */
void audio_plist_move (int i1, int i2)
{
	LOCK (plist_mtx);
	playlist.move(i1, i2);
	UNLOCK (plist_mtx);
}

char *audio_get_mixer_channel_name ()
{
	if (current_mixer == 2)
		return softmixer_name ();

	return hw.get_mixer_channel_name ();
}

void audio_toggle_mixer_channel ()
{
	current_mixer = (current_mixer + 1) % 3;
	if (current_mixer < 2)
		hw.toggle_mixer_channel ();
}
