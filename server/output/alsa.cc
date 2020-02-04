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

/* Based on aplay copyright (c) by Jaroslav Kysela <perex@suse.cz> */

#include <inttypes.h>
#include <alsa/asoundlib.h>

#undef STRERROR_FN 
#define STRERROR_FN alsa_strerror

#include "../audio.h"

#define BUFFER_MAX_USEC	300000

/* ALSA-provided error code to description function wrapper. */
static inline char *alsa_strerror (int errnum)
{
	if (errnum < 0) errnum = -errnum;
	if (errnum < SND_ERROR_BEGIN) return xstrerror (errnum);
	return xstrdup(snd_strerror(errnum));
}

/* Map ALSA's mask to MOC's format (and visa versa). */
static const struct {
	snd_pcm_format_t mask;
	long format;
} format_masks[] = {
	{SND_PCM_FORMAT_S8, SFMT_S8},
	{SND_PCM_FORMAT_U8, SFMT_U8},
	{SND_PCM_FORMAT_S16, SFMT_S16},
	{SND_PCM_FORMAT_U16, SFMT_U16},
	{SND_PCM_FORMAT_S32, SFMT_S32},
	{SND_PCM_FORMAT_U32, SFMT_U32}
};

/* Given an ALSA mask, return a MOC format or zero if unknown. */
static inline long mask_to_format (const snd_pcm_format_mask_t *mask)
{
	long result = 0;

	for (size_t ix = 0; ix < ARRAY_SIZE(format_masks); ix += 1) {
		if (snd_pcm_format_mask_test (mask, format_masks[ix].mask))
			result |= format_masks[ix].format;
	}

	#if 0
	if (snd_pcm_format_mask_test (mask, SND_PCM_FORMAT_S24))
		result |= SFMT_S32; /* conversion needed */
	#endif

	return result;
}

/* Given a MOC format, return an ALSA mask.
 * Return SND_PCM_FORMAT_UNKNOWN if unknown. */
static inline snd_pcm_format_t format_to_mask (long format)
{
	snd_pcm_format_t result = SND_PCM_FORMAT_UNKNOWN;

	for (size_t ix = 0; ix < ARRAY_SIZE(format_masks); ix += 1) {
		if (format_masks[ix].format == format) {
			result = format_masks[ix].mask;
			break;
		}
	}

	return result;
}

#ifndef NDEBUG
static void log_cb(const char *, int, const char *, int, const char *fmt, ...)
{
	assert (fmt);
	va_list va; va_start(va, fmt);
	char *msg = format_msg_va(fmt, va);
	va_end(va);
	logit ("ALSA said: %s", msg);
	free(msg);
}
#endif

static void handle_mixer_events (snd_mixer_t *mixer_handle)
{
	struct pollfd *fds = NULL;

	assert (mixer_handle);

	do {

		int count = snd_mixer_poll_descriptors_count (mixer_handle);
		if (count < 0) {
			log_errno ("snd_mixer_poll_descriptors_count() failed", count);
			break;
		}

		fds = (struct pollfd*) xcalloc (count, sizeof (struct pollfd));

		int rc = snd_mixer_poll_descriptors (mixer_handle, fds, count);
		if (rc < 0) {
			log_errno ("snd_mixer_poll_descriptors() failed", rc);
			break;
		}

		rc = poll (fds, count, 0);
		if (rc < 0) {
			error_errno ("poll() failed", errno);
			break;
		}

		if (rc == 0) break;

		debug ("Mixer event");

		rc = snd_mixer_handle_events (mixer_handle);
		if (rc < 0)
			log_errno ("snd_mixer_handle_events() failed", rc);
	} while (0);

	free (fds);
}

class alsa_driver : public AudioDriver
{
	snd_pcm_t *handle;

	struct {
		unsigned int channels, rate;
		snd_pcm_format_t format;
	} params;

	snd_pcm_uframes_t buffer_frames, chunk_frames;
	int chunk_bytes = -1;
	char buf[512 * 1024];
	int buf_fill = 0;
	int bytes_per_frame, bytes_per_sample;

	snd_mixer_t *mixer_handle;
	snd_mixer_elem_t *mixer_elem1, *mixer_elem2, *mixer_elem_curr;

	/* Percentage volume setting for first and second mixer. */
	mutable int volume1, volume2;

public:
	~alsa_driver()
	{
		close_mixer ();
		
		#ifndef NDEBUG
		snd_lib_error_set_handler (NULL);
		#endif
	}

	alsa_driver(output_driver_caps &caps)
	: handle(NULL)
	, params{ 0, 0, SND_PCM_FORMAT_UNKNOWN }
	, buffer_frames(0), chunk_frames(0)
	, chunk_bytes(-1)
	, buf_fill(0)
	, bytes_per_frame(0), bytes_per_sample(0)
	, mixer_handle(NULL), mixer_elem1(NULL), mixer_elem2(NULL)
	, mixer_elem_curr(NULL)
	, volume1(-1), volume2(-1)
	{
		const char *device = options::ALSADevice.c_str();
		logit ("Initialising ALSA device: %s", device);

		#ifndef NDEBUG
		snd_lib_error_set_handler (log_cb);
		#define RESET_ERR_HANDLER snd_lib_error_set_handler(NULL)
		#else
		#define RESET_ERR_HANDLER do{}while(0)
		#endif

		#define CHECK(tst,msg) if (tst) {\
			error_errno (msg, rc);\
			close_mixer(); RESET_ERR_HANDLER; \
			throw std::runtime_error("ALSA: " msg);}

		int rc = snd_mixer_open (&mixer_handle, 0);
		CHECK(rc < 0, "Can't open ALSA mixer");

		rc = snd_mixer_attach (mixer_handle, device);
		CHECK(rc < 0, "Can't attach mixer");

		rc = snd_mixer_selem_register (mixer_handle, NULL, NULL);
		CHECK(rc < 0, "Can't register mixer");

		rc = snd_mixer_load (mixer_handle);
		CHECK(rc < 0, "Can't load mixer");

		mixer_elem1 = init_mixer_channel (options::ALSAMixer1.c_str());
		mixer_elem2 = init_mixer_channel (options::ALSAMixer2.c_str());
		mixer_elem_curr = mixer_elem1 ? mixer_elem1 : mixer_elem2;
		CHECK(!mixer_elem_curr, "ALSA: failed to init mixer channel");

		int vol;
		if (mixer_elem1 && (vol = read_mixer_raw (mixer_elem1)) != -1) {
			volume1 = vol;
		} else {
			mixer_elem1 = NULL;
			mixer_elem_curr = mixer_elem2;
		}
		if (mixer_elem2 && (vol = read_mixer_raw (mixer_elem2)) != -1) {
			volume2 = vol;
		} else {
			mixer_elem2 = NULL;
			mixer_elem_curr = mixer_elem1;
		}

		CHECK(!fill_capabilities(caps), "ALSA: failed getting capabilities");
	}

	bool open(const sound_params &sound_params) override
	{
		int result = false;
		unsigned int period_time, buffer_time;
		char fmt_name[128];

		assert (!handle);

		params.format = format_to_mask (sound_params.fmt & SFMT_MASK_FORMAT);
		if (params.format == SND_PCM_FORMAT_UNKNOWN) {
			error ("Unknown sample format: %s",
				sfmt_str (sound_params.fmt, fmt_name, sizeof (fmt_name)));
			return false;
		}

		const char *device = options::ALSADevice.c_str();
		logit ("Opening ALSA device: %s", device);

		snd_pcm_hw_params_t *hw_params = open_device (device);
		if (!hw_params) return false;

		int rc = snd_pcm_hw_params_set_access (handle, hw_params,
						SND_PCM_ACCESS_RW_INTERLEAVED);
		if (rc < 0) {
			error_errno ("Can't set ALSA access type", rc);
			goto err;
		}

		rc = snd_pcm_hw_params_set_format (handle, hw_params, params.format);
		if (rc < 0) {
			error_errno ("Can't set sample format", rc);
			goto err;
		}

		bytes_per_sample = sfmt_Bps (sound_params.fmt);
		logit ("Set sample width: %d bytes", bytes_per_sample);

		if (options::ALSAStutterDefeat) {
			rc = snd_pcm_hw_params_set_rate_resample (handle, hw_params, 0);
			if (rc == 0)
				logit ("ALSA resampling disabled");
			else
				log_errno ("Unable to disable ALSA resampling", rc);
		}

		params.rate = sound_params.rate;
		rc = snd_pcm_hw_params_set_rate_near (handle, hw_params, &params.rate, 0);
		if (rc < 0) {
			error_errno ("Can't set sample rate", rc);
			goto err;
		}

		logit ("Set rate: %uHz", params.rate);

		rc = snd_pcm_hw_params_set_channels (handle, hw_params, sound_params.channels);
		if (rc < 0) {
			error_errno ("Can't set number of channels", rc);
			goto err;
		}

		logit ("Set channels: %d", sound_params.channels);

		rc = snd_pcm_hw_params_get_buffer_time_max (hw_params, &buffer_time, 0);
		if (rc < 0) {
			error_errno ("Can't get maximum buffer time", rc);
			goto err;
		}

		buffer_time = MIN(buffer_time, BUFFER_MAX_USEC);
		period_time = buffer_time / 4;

		rc = snd_pcm_hw_params_set_period_time_near (handle, hw_params, &period_time, 0);
		if (rc < 0) {
			error_errno ("Can't set period time", rc);
			goto err;
		}

		rc = snd_pcm_hw_params_set_buffer_time_near (handle, hw_params, &buffer_time, 0);
		if (rc < 0) {
			error_errno ("Can't set buffer time", rc);
			goto err;
		}

		rc = snd_pcm_hw_params (handle, hw_params);
		if (rc < 0) {
			error_errno ("Can't set audio parameters", rc);
			goto err;
		}

		snd_pcm_hw_params_get_period_size (hw_params, &chunk_frames, 0);
		debug ("Chunk size: %lu frames", chunk_frames);

		snd_pcm_hw_params_get_buffer_size (hw_params, &buffer_frames);
		debug ("Buffer size: %lu frames", buffer_frames);
		debug ("Buffer time: %" PRIu64"us",
			(uint64_t) buffer_frames * UINT64_C(1000000) / params.rate);

		bytes_per_frame = sound_params.channels * bytes_per_sample;
		debug ("Frame size: %d bytes", bytes_per_frame);

		chunk_bytes = chunk_frames * bytes_per_frame;

		if (chunk_frames == buffer_frames) {
			error ("Can't use period equal to buffer size (%lu == %lu)",
					chunk_frames, buffer_frames);
			goto err;
		}

		rc = snd_pcm_prepare (handle);
		if (rc < 0) {
			error_errno ("Can't prepare audio interface for use", rc);
			goto err;
		}

		/* Check that ALSA's and MOC's byte/sample/frame conversions agree. */
		#ifndef NDEBUG
		# define ALSA_CHECK(fn,val) \
		do { \
			long v = val; \
			ssize_t ssz = snd_pcm_##fn (handle, 1); \
			if (ssz < 0) \
				debug ("CHECK: snd_pcm_%s() failed: %s", #fn, alsa_strerror (ssz)); \
			else if (v != ssz) \
				debug ("CHECK: snd_pcm_%s() = %zd (vs %ld)", #fn, ssz, v); \
		} while (0)
		ALSA_CHECK (samples_to_bytes, bytes_per_sample);
		ALSA_CHECK (frames_to_bytes, bytes_per_frame);
		#undef ALSA_CHECK
		#endif

		logit ("ALSA device opened");

		params.channels = sound_params.channels;
		buf_fill = 0;
		result = 1;

	err:
		snd_pcm_hw_params_free (hw_params);

		return result;
	}

	void close() override
	{
		if (!handle) return;

		/* play what remained in the buffer */
		if (buf_fill > 0) {
			unsigned int samples_required;

			assert (buf_fill < chunk_bytes);

			samples_required = (chunk_bytes - buf_fill) / bytes_per_sample;
			snd_pcm_format_set_silence (params.format, buf + buf_fill,
										samples_required);

			buf_fill = chunk_bytes;
			play_buf_chunks ();
		}

		/* Wait for ALSA buffers to empty.
		* Do not be tempted to use snd_pcm_nonblock() and snd_pcm_drain()
		* here; there are two bugs in ALSA which make it a bad idea (see
		* the SVN commit log for r2550).  Instead we sleep for the duration
		* of the still unplayed samples. */
		snd_pcm_sframes_t delay;
		if (snd_pcm_delay (handle, &delay) == 0 && delay > 0)
			xsleep (delay, params.rate);
		snd_pcm_close (handle);
		logit ("ALSA device closed");

		params.format = (snd_pcm_format_t)0;
		params.rate = 0;
		params.channels = 0;
		buffer_frames = 0;
		chunk_frames = 0;
		chunk_bytes = -1;
		handle = NULL;
	}

	int play (const char *buff, size_t size) override
	{
		int to_write = size;
		int buf_pos = 0;

		assert (chunk_bytes > 0);

		while (to_write) {
			int to_copy = MIN(to_write, ssizeof(buf) - buf_fill);
			memcpy (buf + buf_fill, buff + buf_pos, to_copy);
			to_write -= to_copy;
			buf_pos += to_copy;
			buf_fill += to_copy;

			if (play_buf_chunks() < 0) return -1;
		}

		return size;
	}

	int read_mixer () const override
	{
		int actual_vol = read_mixer_raw (mixer_elem_curr);

		int &vol = (mixer_elem_curr == mixer_elem1 ? volume1 : volume2);
		if (vol != actual_vol) {
			vol = actual_vol;
			logit ("Mixer volume has changed since we last read it.");
		}

		return actual_vol;
	}

	void set_mixer (int vol) override
	{
		if (!mixer_handle) return;

		debug ("Setting vol to %d", vol);

		(mixer_elem_curr == mixer_elem1 ? volume1 : volume2) = vol;
		int rc = snd_mixer_selem_set_playback_volume_all (mixer_elem_curr, vol);
		if (rc < 0) error_errno ("Can't set mixer", rc);
	}

	int get_buff_fill () const override
	{
		if (!handle) return 0;

		snd_pcm_sframes_t delay;
		int rc = snd_pcm_delay (handle, &delay);
		if (rc < 0) {
			log_errno ("snd_pcm_delay() failed", rc);
			return 0;
		}

		/* delay can be negative if an underrun occurs */
		return MAX(delay, 0) * bytes_per_frame;
	}

	bool reset () override
	{
		if (!handle) {
			logit ("reset() when the device is not opened.");
			return false;
		}

		int rc = snd_pcm_drop (handle);
		if (rc < 0) {
			error_errno ("Can't reset the device", rc);
			return false;
		}

		rc = snd_pcm_prepare (handle);
		if (rc < 0) {
			error_errno ("Can't prepare after reset", rc);
			return false;
		}

		buf_fill = 0;
		return true;
	}

	int get_rate () const override
	{
		return params.rate;
	}

	void toggle_mixer_channel () override
	{
		if (mixer_elem_curr == mixer_elem1 && mixer_elem2)
			mixer_elem_curr = mixer_elem2;
		else if (mixer_elem1)
			mixer_elem_curr = mixer_elem1;
	}

	str get_mixer_channel_name() const override
	{
		if (mixer_elem_curr == mixer_elem1)
			return options::ALSAMixer1;
		else
			return options::ALSAMixer2;
	}

private:
	snd_pcm_hw_params_t *open_device (const char *device)
	{
		assert (!handle);

		int rc = snd_pcm_open (&handle, device, SND_PCM_STREAM_PLAYBACK,
						SND_PCM_NONBLOCK);
		if (rc < 0) {
			error_errno ("Can't open audio", rc);
			goto err1;
		}

		snd_pcm_hw_params_t *result;
		rc = snd_pcm_hw_params_malloc (&result);
		if (rc < 0) {
			error_errno ("Can't allocate hardware parameters structure", rc);
			goto err2;
		}

		rc = snd_pcm_hw_params_any (handle, result);
		if (rc < 0) {
			error_errno ("Can't initialize hardware parameters structure", rc);
			goto err3;
		}

		if (0) {
		err3:
			snd_pcm_hw_params_free (result);
		err2:
			snd_pcm_close (handle);
		err1:
			result = NULL;
			handle = NULL;
		}

		return result;
	}

	/* Fill caps with the device capabilities. Return 0 on error. */
	bool fill_capabilities (output_driver_caps &caps)
	{
		assert (!handle);

		snd_pcm_hw_params_t *hw_params = open_device (options::ALSADevice.c_str());
		if (!hw_params) return 0;

		bool result = false;
		do {
			unsigned int val;
			int rc = snd_pcm_hw_params_get_channels_min (hw_params, &val);
			if (rc < 0) {
				error_errno ("Can't get the minimum number of channels", rc);
				break;
			}
			caps.min_channels = val;

			rc = snd_pcm_hw_params_get_channels_max (hw_params, &val);
			if (rc < 0) {
				error_errno ("Can't get the maximum number of channels", rc);
				break;
			}
			caps.max_channels = val;

			snd_pcm_format_mask_t *format_mask;
			rc = snd_pcm_format_mask_malloc (&format_mask);
			if (rc < 0) {
				error_errno ("Can't allocate format mask", rc);
				break;
			}
			snd_pcm_hw_params_get_format_mask (hw_params, format_mask);
			caps.formats = mask_to_format (format_mask) | SFMT_NE;
			snd_pcm_format_mask_free (format_mask);

			result = true;
		} while (0);

		snd_pcm_hw_params_free (hw_params);
		snd_pcm_close (handle);
		handle = NULL;

		return result;
	}

	/* Play from buf as many chunks as possible. Move the remaining data
	* to the beginning of the buffer. Return the number of bytes written
	* or -1 on error. */
	int play_buf_chunks ()
	{
		int written = 0;
		bool zero_logged = false;

		while (buf_fill >= chunk_bytes) {
			int rc;

			rc = snd_pcm_writei (handle, buf + written, chunk_frames);

			if (rc == 0) {
				if (!zero_logged) {
					debug ("Played 0 bytes");
					zero_logged = true;
				}
				continue;
			}

			zero_logged = false;

			if (rc > 0) {
				int written_bytes = rc * bytes_per_frame;

				written += written_bytes;
				buf_fill -= written_bytes;

				continue;
			}

			rc = snd_pcm_recover (handle, rc, 0);

			switch (rc) {
			case 0:
				break;
			case -EAGAIN:
				if (snd_pcm_wait (handle, 500) < 0)
					logit ("snd_pcm_wait() failed");
				break;
			default:
				error_errno ("Can't play", rc);
				return -1;
			}
		}

		memmove (buf, buf + written, buf_fill);

		return written;
	}

	int read_mixer_raw (snd_mixer_elem_t *elem) const
	{
		int rc, nchannels = 0, volume = 0;
		bool joined;
		snd_mixer_selem_channel_id_t chan_id;

		if (!mixer_handle) return -1;

		assert (elem);

		handle_mixer_events (mixer_handle);

		joined = snd_mixer_selem_has_playback_volume_joined (elem);

		for (chan_id = (snd_mixer_selem_channel_id_t)0; chan_id < SND_MIXER_SCHN_LAST; chan_id = (snd_mixer_selem_channel_id_t)(chan_id + 1)) {
			if (snd_mixer_selem_has_playback_channel (elem, chan_id)) {
				long vol;

				nchannels += 1;
				rc = snd_mixer_selem_get_playback_volume (elem, chan_id, &vol);
				if (rc < 0) {
					error_errno ("Can't read mixer", rc);
					return -1;
				}

				#if 0
				{
					static int prev_vol[SND_MIXER_SCHN_LAST] = {0};

					if (vol != prev_vol[chan_id]) {
						prev_vol[chan_id] = vol;
						debug ("Vol %d: %ld", chan_id, vol);
					}
				}
				#endif

				volume += vol;
			}

			if (joined)
				break;
		}

		if (nchannels == 0) {
			logit ("Mixer has no channels");
			return -1;
		}

		volume /= nchannels;

		return volume;
	}

	snd_mixer_elem_t *init_mixer_channel (const char *name)
	{
		snd_mixer_selem_id_t *sid;
		snd_mixer_elem_t *result = NULL;

		assert (mixer_handle);

		snd_mixer_selem_id_malloc (&sid);
		snd_mixer_selem_id_set_index (sid, 0);
		snd_mixer_selem_id_set_name (sid, name);

		do {
			snd_mixer_elem_t *elem = NULL;

			elem = snd_mixer_find_selem (mixer_handle, sid);
			if (!elem) {
				error ("Can't find mixer %s", name);
				break;
			}

			if (!snd_mixer_selem_has_playback_volume (elem)) {
				error ("Mixer device has no playback volume (%s).", name);
				break;
			}

			if (snd_mixer_selem_set_playback_volume_range (elem, 0, 100) < 0) {
				error ("Cannot set playback volume range (%s).", name);
				break;
			}

			logit ("Opened mixer (%s)", name);
			result = elem;
		} while (0);

		snd_mixer_selem_id_free (sid);

		return result;
	}

	void close_mixer ()
	{
		if (!mixer_handle) return;
			
		int rc = snd_mixer_close (mixer_handle);
		if (rc < 0) log_errno ("Can't close mixer", rc);

		mixer_handle = NULL;
	}
};

AudioDriver *ALSA_init(output_driver_caps &caps)
{
	try
	{
		return new alsa_driver(caps);
	}
	catch(...) {}
	
	return NULL;
}
