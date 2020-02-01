/*
 * MOC - music on console
 *
 * SNDIO sound driver for MOC by Alexander Polakov.
 * Copyright (C) 2011 Alexander Polakov <polachok@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

# include <sndio.h>
#include "../audio.h"

#define PCT_TO_SIO(pct)	((127 * (pct) + 50) / 100)
#define SIO_TO_PCT(vol)	((100 * (vol) + 64) / 127)

static void volume_cb (void *drv, unsigned int vol);

struct sndio_driver : public AudioDriver
{
	sio_hdl *hdl;
	int curvol;
	struct sound_params params;

	sndio_driver(output_driver_caps &caps)
	: hdl(NULL)
	, curvol(100)
	, params { 0, 0, 0 }
	{
		caps.formats = SFMT_S8 | SFMT_U8 | SFMT_U16 | SFMT_S16 | SFMT_NE;
		caps.min_channels = 1;
		caps.max_channels = 2;
	}

	~sndio_driver()
	{
		if (hdl) close ();
	}

	bool open(const sound_params &sound_params) override
	{
		assert (hdl == NULL);
		hdl = sio_open (NULL, SIO_PLAY, 0);
		if (!hdl) return false;

		params = sound_params;
		struct sio_par par; sio_initpar (&par);
		/* Add volume change callback. */
		sio_onvol (hdl, volume_cb, this);
		par.rate = sound_params.rate;
		par.pchan = sound_params.channels;
		par.bits = (sound_params.fmt & (SFMT_S8|SFMT_U8) ? 8 : 16);
		par.le = SIO_LE_NATIVE;
		par.sig = (sound_params.fmt & (SFMT_S16|SFMT_S8) ? 1 : 0);
		par.round = par.rate / 8;
		par.appbufsz = par.round * 2;
		logit ("rate %d pchan %d bits %d sign %d", par.rate, par.pchan, par.bits, par.sig);

		if (!sio_setpar (hdl, &par) || !sio_getpar (hdl, &par) || !sio_start (hdl)) {
			logit ("Failed to set sndio parameters.");
			sio_close (hdl);
			hdl = NULL;
			return false;
		}
		sio_setvol (hdl, PCT_TO_SIO(curvol));
		return true;
	}

	/* Return the number of bytes played, or -1 on error. */
	int play (const char *buff, const size_t size) override
	{
		assert (hdl != NULL);
		int count = (int) sio_write (hdl, buff, size);
		return (!count && sio_eof(hdl) ? -1 : count);
	}

	void close () override
	{
		if (!hdl) return;
		sio_stop (hdl);
		sio_close (hdl);
		hdl = NULL;
	}

	int read_mixer () const override
	{
		return curvol;
	}

	void set_mixer (int vol) override
	{
		if (hdl) sio_setvol (hdl, PCT_TO_SIO (vol));
	}

	int get_buff_fill () const override
	{
		/* Since we cannot stop SNDIO playing the samples already in
		* its buffer, there will never be anything left unheard. */
		return 0;
	}

	bool reset () override
	{
		/* SNDIO will continue to play the samples already in its buffer
		* regardless of what we do, so there's nothing we can do. */

		return true;
	}

	int get_rate () const override
	{
		return params.rate;
	}

	void toggle_mixer_channel () override
	{
	}

	str get_mixer_channel_name () const override
	{
		return "moc";
	}
};

static void volume_cb (void *drv, unsigned int vol)
{
	((sndio_driver*)drv)->curvol = SIO_TO_PCT(vol);
}

AudioDriver *SNDIO_init(output_driver_caps &caps)
{
	return new sndio_driver(caps);
}
