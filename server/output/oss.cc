/*
 * MOC - music on console
 * Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
# include <sys/soundcard.h>

#include "../server.h"
#include "../audio.h"

#if OSS_VERSION < 0x40000 && SOUND_VERSION < 0x40000
#define OSSv3_MIXER
#endif

static const struct {
	const char *name;
	const int num;
} mixer_channels[] = {
	{ "pcm", SOUND_MIXER_PCM },
	{ "master", SOUND_MIXER_VOLUME },
	{ "speaker", SOUND_MIXER_SPEAKER }
};
#define MIXER_CHANNELS_NUM (ARRAY_SIZE(mixer_channels))

static int mixer_name_to_channel (const char *name)
{
	for (size_t ix = 0; ix < MIXER_CHANNELS_NUM; ix += 1) {
		if (!strcasecmp (mixer_channels[ix].name, name))
			return ix;
	}
	return -1;
}

struct oss_driver : public AudioDriver
{
	bool started;
	volatile int dsp_fd;
	sound_params params;
	#ifdef OSSv3_MIXER
	int mixer_fd;
	int mixer_channel1;
	int mixer_channel2;
	int mixer_channel_current;
	#endif

	bool open_dev ()
	{
		if ((dsp_fd = ::open (options::OSSDevice.c_str(), O_WRONLY)) == -1) {
			char *err = xstrerror (errno);
			error ("Can't open %s: %s", options::OSSDevice.c_str(), err);
			free (err);
			return false;
		}
		logit ("Audio device opened");
		return true;
	}

	/* Fill caps with the device capabilities.  Return 0 on error. */
	bool set_capabilities (output_driver_caps &caps)
	{
		if (!open_dev ()) {
			error ("Can't open the device.");
			return false;
		}

		int format_mask;
		if (ioctl (dsp_fd, SNDCTL_DSP_GETFMTS, &format_mask) == -1) {
			error_errno ("Can't get supported audio formats", errno);
			::close (dsp_fd);
			return 0;
		}

		caps.formats = 0;
		if (format_mask & AFMT_S8) caps.formats |= SFMT_S8;
		if (format_mask & AFMT_U8) caps.formats |= SFMT_U8;

		if (format_mask & AFMT_S16_LE) caps.formats |= SFMT_S16 | SFMT_LE;
		if (format_mask & AFMT_S16_BE) caps.formats |= SFMT_S16 | SFMT_BE;

		#if defined(AFMT_S32_LE) && defined(AFMT_S32_BE)
		if (format_mask & AFMT_S32_LE) caps.formats |= SFMT_S32 | SFMT_LE;
		if (format_mask & AFMT_S32_BE) caps.formats |= SFMT_S32 | SFMT_BE;
		#endif

		if (!caps.formats) {
			/* Workaround for vmix which lies that it doesn't support any
			* format. */
			error ("The driver claims that no format known to me is "
			"supported. I will assume that SFMT_S8 and "
			"SFMT_S16 (native endian) are supported.");
			caps.formats = SFMT_S8 | SFMT_S16 | SFMT_NE;
		}

		caps.min_channels = caps.max_channels = 1;
		if (ioctl (dsp_fd, SNDCTL_DSP_CHANNELS, &caps.min_channels)) {
			error_errno ("Can't set number of channels", errno);
			::close (dsp_fd);
			return 0;
		}

		::close (dsp_fd);
		if (!open_dev ()) {
			error ("Can't open the device.");
			return 0;
		}

		if (caps.min_channels != 1)
			caps.min_channels = 2;
		caps.max_channels = 2;
		if (ioctl (dsp_fd, SNDCTL_DSP_CHANNELS, &caps.max_channels)) {
			error_errno ("Can't set number of channels", errno);
			::close (dsp_fd);
			return 0;
		}

		if (caps.max_channels != 2) {
			if (caps.min_channels == 2) {
				error ("Can't get any supported number of channels.");
				::close (dsp_fd);
				return 0;
			}
			caps.max_channels = 1;
		}

		::close (dsp_fd);

		return 1;
	}

	/* Get PCM volume.  Return -1 on error. */
	int read_mixer () const override
	{
		if (!started) return -1;

		int vol;
		#ifdef OSSv3_MIXER
		if (mixer_fd != -1 && mixer_channel_current != -1) {
			if (ioctl (mixer_fd, MIXER_READ(mixer_channel_current), &vol) == -1)
		#else
		if (dsp_fd != -1) {
			if (ioctl (dsp_fd, SNDCTL_DSP_GETPLAYVOL, &vol) == -1)
		#endif
			{
				error ("Can't read from mixer");
			}
			else {
				/* Average between left and right */
				return ((vol & 0xFF) + ((vol >> 8) & 0xFF)) / 2;
			}
		}

		return -1;
	}

	oss_driver(output_driver_caps &caps)
	: started(false)
	, dsp_fd(-1)
	, params { 0, 0, 0 }
	#ifdef OSSv3_MIXER
	, mixer_fd(-1)
	, mixer_channel1(-1)
	, mixer_channel2(-1)
	#endif
	{
	#ifdef OSSv3_MIXER
		/* Open the mixer device */
		mixer_fd = ::open (options::OSSMixerDevice.c_str(), O_RDWR);
		if (mixer_fd == -1) {
			char *err = xstrerror (errno);
			error ("Can't open mixer device %s: %s",
				options::OSSMixerDevice.c_str(), err);
			free (err);
		}
		else {
			mixer_channel1 = mixer_name_to_channel (options::OSSMixerChannel1.c_str());
			mixer_channel2 = mixer_name_to_channel (options::OSSMixerChannel2.c_str());

			if (mixer_channel1 == -1) fatal ("Bad first OSS mixer channel!");
			if (mixer_channel2 == -1) fatal ("Bad second OSS mixer channel!");

			/* test mixer channels */
			mixer_channel_current = mixer_channel1;
			if (read_mixer () == -1)
				mixer_channel1 = -1;

			mixer_channel_current = mixer_channel2;
			if (read_mixer () == -1)
				mixer_channel2 = -1;

			if (mixer_channel1 != -1)
				mixer_channel_current = mixer_channel1;
		}
	#endif
		if (!set_capabilities (caps))
			throw std::runtime_error("OSS init failed");
	}

	~oss_driver()
	{
		#ifdef OSSv3_MIXER
		if (mixer_fd != -1) {
			::close (mixer_fd);
			mixer_fd = -1;
		}
		#endif
	}

	void close () override
	{
		if (dsp_fd != -1) {
			::close (dsp_fd);
			dsp_fd = -1;
			logit ("Audio device closed");
		}

		started = false;
		params.channels = 0;
		params.rate = 0;
		params.fmt = 0;
	}

	/* Return 0 on error. */
	int set_params ()
	{
		int req_format;
		int req_channels;
		char fmt_name[SFMT_STR_MAX];

		/* Set format */
		switch (params.fmt & SFMT_MASK_FORMAT) {
			case SFMT_S8:
				req_format = AFMT_S8;
				break;
			case SFMT_U8:
				req_format = AFMT_U8;
				break;
			case SFMT_S16:
				if (params.fmt & SFMT_LE)
					req_format = AFMT_S16_LE;
				else
					req_format = AFMT_S16_BE;
				break;
			#if defined(AFMT_S32_LE) && defined(AFMT_S32_BE)
			case SFMT_S32:
				if (params.fmt & SFMT_LE)
					req_format = AFMT_S32_LE;
				else
					req_format = AFMT_S32_BE;
				break;
			#endif
			default:
				error ("Format %s is not supported by the device",
					sfmt_str (params.fmt, fmt_name, sizeof (fmt_name)));
				return 0;
		}

		if (ioctl (dsp_fd, SNDCTL_DSP_SETFMT, &req_format) == -1) {
			error_errno ("Can't set audio format", errno);
			close ();
			return 0;
		}

		/* Set number of channels */
		req_channels = params.channels;
		if (ioctl (dsp_fd, SNDCTL_DSP_CHANNELS, &req_channels) == -1) {
			char *err = xstrerror (errno);
			error ("Can't set number of channels to %d: %s",
				params.channels, err);
			free (err);
			close ();
			return 0;
		}
		if (params.channels != req_channels) {
			error ("Can't set number of channels to %d, "
			"device doesn't support this value",
					params.channels);
			close ();
			return 0;
		}

		/* Set sample rate */
		if (ioctl (dsp_fd, SNDCTL_DSP_SPEED, &params.rate) == -1) {
			char *err = xstrerror (errno);
			error ("Can't set sampling rate to %d: %s", params.rate, err);
			free (err);
			close ();
			return 0;
		}

		logit ("Audio parameters set to: %s, %d channels, %dHz",
			sfmt_str (params.fmt, fmt_name, sizeof (fmt_name)),
			params.channels, params.rate);

		return 1;
	}

	/* Return 0 on failure. */
	bool open (const sound_params &sound_params) override
	{
		params = sound_params;

		if (!open_dev ()) return false;

		if (!set_params ()) {
			close ();
			return false;
		}

		started = true;
		return true;
	}

	/* Return -1 on error, or number of bytes played when okay. */
	int play (const char *buff, size_t size) override
	{
		ssize_t ssize = (ssize_t) size;
		ssize_t count = 0;

		if (dsp_fd == -1) {
			error ("Can't play: audio device isn't opened!");
			return -1;
		}

		while (count < ssize) {
			ssize_t rc;

			rc = write (dsp_fd, buff + count, ssize - count);
			if (rc == -1) {
				error_errno ("Error writing pcm sound", errno);
				return -1;
			}

			count += rc;
		}

		return count;
	}

	/* Set PCM volume */
	void set_mixer (int vol) override
	{
	#ifdef OSSv3_MIXER
		if (mixer_fd == -1) return;
	#else
		if (dsp_fd == -1) return;
	#endif
		vol = CLAMP(0, vol, 100);
		vol |= vol << 8;
	#ifdef OSSv3_MIXER
		if (ioctl (mixer_fd, MIXER_WRITE(mixer_channel_current), &vol) == -1)
	#else
		if (ioctl (dsp_fd, SNDCTL_DSP_SETPLAYVOL, &vol) == -1)
	#endif
		{
			error ("Can't set mixer: ioctl() failed");
		}
	}

	/* Return number of bytes in device buffer. */
	int get_buff_fill () const override
	{
		audio_buf_info buff_info;

		if (dsp_fd == -1) return 0;

		if (ioctl (dsp_fd, SNDCTL_DSP_GETOSPACE, &buff_info) == -1) {
			error ("SNDCTL_DSP_GETOSPACE failed");
			return false;
		}

		return (buff_info.fragstotal * buff_info.fragsize) - buff_info.bytes;
	}

	/* Reset device buffer and stop playing immediately.  Return 0 on error. */
	bool reset () override
	{
		if (dsp_fd == -1) {
			logit ("Reset when audio device is not opened");
			return false;
		}

		logit ("Resetting audio device");

		if (ioctl (dsp_fd, SNDCTL_DSP_RESET, NULL) == -1)
			error ("Resetting audio device failed");
		::close (dsp_fd);
		dsp_fd = -1;
		if (!open_dev () || !set_params ()) {
			error ("Failed to open audio device after resetting");
			return false;
		}

		return true;
	}

	void toggle_mixer_channel () override
	{
		#ifdef OSSv3_MIXER
		if (mixer_channel_current == mixer_channel1 && mixer_channel2 != -1)
			mixer_channel_current = mixer_channel2;
		else if (mixer_channel1 != -1)
			mixer_channel_current = mixer_channel1;
		#endif
	}

	str get_mixer_channel_name () const override
	{
		#ifdef OSSv3_MIXER
			if (mixer_channel_current == mixer_channel1)
				return options::OSSMixerChannel1;
			return options::OSSMixerChannel2;
		#else
			return "moc";
		#endif
	}

	int get_rate () const override
	{
		return params.rate;
	}
};

AudioDriver *OSS_init(output_driver_caps &caps)
{
	try
	{
		return new oss_driver(caps);
	}
	catch(...) {}
	
	return NULL;
}
