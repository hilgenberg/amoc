/*
 * MOC - music on console
 * Copyright (C) 2004-2008 Damian Pietras <daper@daper.net>
 *
 * Equalizer-extension Copyright (C) 2008 Hendrik Iben <hiben@tzi.de>
 * Provides a parametric biquadratic equalizer.
 *
 * This code is based on the 'Cookbook formulae for audio EQ biquad filter
 * coefficients' by Robert Bristow-Johnson.
 * https://www.w3.org/2011/audio/audio-eq-cookbook.html
 *
 * TODO:
 * - Merge somehow with softmixer code to avoid multiple endianness
 *   conversions.
 * - Implement equalizer routines for integer samples... conversion
 *   to float (and back) is lazy...
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <locale.h>

#include "../audio.h"
#include "audio_conversion.h"
#include "equalizer.h"

struct Biquad
{
	float a0, a1, a2, a3, a4;
	mutable float x1, x2, y1, y2;

	/* biquad functions */

	/* Create a Peaking EQ Filter.
	* See 'Audio EQ Cookbook' for more information
	*/
	Biquad(float dbgain, float cf, float srate, float bw)
	{
		float A = powf(10.0f, dbgain / 40.0f);
		float omega = 2.0 * M_PI * cf / srate;
		float sn = sin(omega);
		float cs = cos(omega);
		float alpha = sn * sinh(M_LN2 / 2.0f * bw * omega / sn);

		float alpha_m_A = alpha * A;
		float alpha_d_A = alpha / A;

		float b0 =  1.0f + alpha_m_A;
		float b1 = -2.0f * cs;
		float b2 =  1.0f - alpha_m_A;
		float a0 =  1.0f + alpha_d_A;
		float a1 =  b1;
		float a2 =  1.0f - alpha_d_A;

		this->a0 = b0 / a0;
		this->a1 = b1 / a0;
		this->a2 = b2 / a0;
		this->a3 = a1 / a0;
		this->a4 = a2 / a0;

		this->x1 = 0.0f;
		this->x2 = 0.0f;
		this->y1 = 0.0f;
		this->y2 = 0.0f;
	}

	void operator() (float &s, float &f) const
	{
		f = s*a0 + a1*x1 + a2*x2 - a3*y1 - a4*y2;
		x2 = x1; x1 = s;
		y2 = y1; y1 = f;
		s = f;
	}
};

struct Equalizer
{
	str   name;
	float preamp;
	std::vector<Biquad> b;
};

/* config processing */
static int read_setup(char *desc, Equalizer *&sp);

static std::vector<std::unique_ptr<Equalizer>> equalizers;
static int       eqi = -1;
static Equalizer *eq = NULL;

static int sample_rate, channels;
static float mixin_rate;

bool equalizer_is_active() { return options::EqualizerActive; }
void equalizer_set_active(bool active) { options::EqualizerActive = active; }
str equalizer_current_eqname()
{
	return (options::EqualizerActive && eq) ? eq->name : str("off");
}

void equalizer_next()
{
	if (equalizers.empty()) return;
	++eqi;
	eqi %= equalizers.size(); eq = equalizers[eqi].get();
	options::EqualizerPreset = eq->name;
}

void equalizer_prev()
{
	if (equalizers.empty()) return;
	eqi += equalizers.size()-1;
	eqi %= equalizers.size(); eq = equalizers[eqi].get();
	options::EqualizerPreset = eq->name;
}

/* Applies a set of biquadratic filters to a buffer of floating point samples.
 * It is safe to have the same input and output buffer.
 * length of src and dst is len
 */
static inline void apply_biquads(float *src, float *dst, int len, const std::vector<Biquad> b)
{
	int blen = b.size() / channels;
	while (len > 0)
	{
		for (int c = 0; c < channels; ++c)
		{
			auto *cb = b.data() + c*blen; // data for channel c
			float s = *src++, f = s;
			for (int i = 0; i < blen; ++i) (*cb++)(s, f);
			*dst++ = f;
			--len;
		}
	}
}

void equalizer_init()
{
	sample_rate = 44100;
	channels = 2;
	mixin_rate = 0.25f;
	equalizer_refresh();
	logit ("Equalizer initialized");
}

void equalizer_shutdown()
{
	equalizers.clear();
	logit ("Equalizer stopped");
}

void equalizer_refresh()
{
	equalizers.clear();

	eqi = -1;
	eq  = NULL;

	DIR *d = opendir(options::config_file_path("eqsets").c_str());
	if(!d) return;

	struct dirent *de;
	while((de = readdir(d)))
	{
		if (!de->d_name || de->d_name[0] == '.') continue;
		str filename = options::config_file_path(format("eqsets/%s", de->d_name).c_str());

		struct stat st;
		stat(filename.c_str(), &st);
		if(!S_ISREG(st.st_mode)) continue;
		
		FILE *f = fopen(filename.c_str(), "r");
		if (!f) continue;
		
		char filebuffer[4096];

		char *fb = filebuffer;

		int maxread = 4095 - (fb - filebuffer);

		// read in whole file
		while(!feof(f) && maxread>0)
		{
			maxread = 4095 - (fb - filebuffer);
			int rb = fread(fb, sizeof(char), maxread, f);
			fb+=rb;
		}
		*fb = 0;

		fclose(f);

		Equalizer *new_eq = NULL;
		int r = read_setup(filebuffer, new_eq);
		switch(r)
		{
			case 0:
				new_eq->name = de->d_name;
				equalizers.emplace_back(new_eq);
				if (new_eq->name == options::EqualizerPreset)
				{
					eqi = equalizers.size()-1;
					eq = new_eq;
				}
				break;
			case -1:
				logit ("Not an EQSET (empty file): %s", filename);
				break;
			case -2:
				logit ("Not an EQSET (invalid header): %s", filename);
				break;
			case -3:
				logit ("Error while parsing settings from EQSET: %s", filename);
				break;
			default:
				logit ("Unknown error while parsing EQSET: %s", filename);
				break;
		}
	}

	closedir(d);

	if (!eq) equalizer_next();
}

template<typename T>
static void process(T *buf, size_t samples)
{
	constexpr auto A = (float)std::numeric_limits<T>::min();
	constexpr auto B = (float)std::numeric_limits<T>::max();
	float *tmp = (float *)xmalloc (samples * sizeof (float));

	for (size_t i = 0; i < samples; ++i)
		tmp[i] = eq->preamp * (float)buf[i];

	apply_biquads(tmp, tmp, samples, eq->b);

	for (size_t i = 0; i < samples; ++i)
	{
		tmp[i] = (1.0f - mixin_rate) * tmp[i] + mixin_rate * (float)buf[i];
		buf[i] = (T)CLAMP(A, tmp[i], B);
	}

	free(tmp);
}
static void process(float *buf, size_t samples)
{
	float *tmp = (float *)xmalloc (samples * sizeof (float));

	for (size_t i = 0; i < samples; ++i)
		tmp[i] = eq->preamp * (float)buf[i];

	apply_biquads(tmp, tmp, samples, eq->b);

	for (size_t i = 0; i < samples; ++i)
	{
		tmp[i] = (1.0f - mixin_rate) * tmp[i] + mixin_rate * buf[i];
		buf[i] = CLAMP(-1.0f, tmp[i], 1.0f);
	}

	free(tmp);
}

/* sound processing code */
void equalizer_process_buffer(char *buf, size_t size, const sound_params &sp)
{
	if(!options::EqualizerActive || !eq || eq->b.empty()) return;

	if (sp.rate != sample_rate || sp.channels != channels)
	{
		logit ("Recreating filters due to sound parameter changes...");
		sample_rate = sp.rate;
		channels = sp.channels;
		equalizer_refresh();
	}

	bool do_endian = (sp.fmt & SFMT_MASK_ENDIANNESS != SFMT_NE);

	switch (sp.fmt & SFMT_MASK_FORMAT)
	{
		case SFMT_U8:
			process((uint8_t *)buf, size);
			break;
		case SFMT_S8:
			process((int8_t *)buf, size);
			break;
		case SFMT_U16:
			size /= sizeof(uint16_t);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			process((uint16_t *)buf, size);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			break;
		case SFMT_S16:
			size /= sizeof(int16_t);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			process((int16_t *)buf, size);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			break;
		case SFMT_U32:
			size /= sizeof(uint32_t);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			process((uint32_t *)buf, size);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			break;
		case SFMT_S32:
			size /= sizeof(int32_t);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			process((int32_t *)buf, size);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			break;
		case SFMT_FLOAT:
			size /= sizeof(float);
			process((float *)buf, size);
			break;
	}
}

/* parsing stuff */

static char *skip_line(char *s)
{
	while(*s && *s!='\r' && *s!='\n') s++;
	bool dos_line = (*s=='\r');
	if(*s) s++;
	if(dos_line && *s=='\n') s++;
	return s;
}

static char *skip_whitespace(char *s)
{
	while(*s && *s<=' ') s++;
	if(!*s) return s;

	if(*s=='#')
	{
		s = skip_line(s);
		s = skip_whitespace(s);
	}
	return s;
}

static int read_float(char *s, float *f, char **endp)
{
	errno = 0;
	float t = strtof(s, endp);
	if(errno==ERANGE) return -1;
	if(*endp == s) return -2;
	*f = t;
	return 0;
}

static int read_setup(char *desc, Equalizer *&s)
{
	char *curloc = setlocale(LC_NUMERIC, NULL);
	setlocale(LC_NUMERIC, "C"); // posix decimal point

	desc = skip_whitespace(desc);

	if(!*desc) return -1;

	if(strncasecmp(desc, "EQSET", 5)) return -2;
	desc += 5;

	desc = skip_whitespace(skip_line(desc));

	float preamp = 0.0f;
	std::vector<float> CF, BW, DG;

	int r;

	while(*desc)
	{
		char *endp;

		float cf = 0.0f;

		r = read_float(desc, &cf, &endp);
		if(r!=0) return -3;

		desc = skip_whitespace(endp);

		float bw = 0.0f;

		r = read_float(desc, &bw, &endp);
		if(r!=0) return -3;

		desc = skip_whitespace(endp);

		/* 0Hz means preamp, only one parameter then */
		if(cf!=0.0f)
		{
			float dg = 0.0f;
			r = read_float(desc, &dg, &endp);
			if(r!=0) return -3;

			desc = skip_whitespace(endp);

			CF.push_back(cf);
			BW.push_back(bw);
			DG.push_back(dg);
		}
		else
		{
			preamp = bw;
		}
	}

	if (curloc) setlocale(LC_NUMERIC, curloc); // posix decimal point

	const int N = CF.size();
	if (!N) return -4;

	s = new Equalizer;
	s->b.reserve(channels * N);

	for (int i = 0; i < N; ++i)
		s->b.emplace_back(DG[i], CF[i], sample_rate, BW[i]);
	for (int c = 1; c < channels; ++c)
		s->b.insert(s->b.end(), s->b.begin(), s->b.begin() + N);

	/*
	preamping
	XMMS / Beep Media Player / Audacious use all the same code but
	do something I do not understand for preamping...

	actually preamping by X dB should be like
	sample * 10^(X/20)

	they do:
	sample * (( 1.0 + 0.0932471 * X + 0.00279033 * X^2 ) / 2)

	what are these constants ?
	the equations are not even close to each other in their results...
	- hiben
	*/
	s->preamp = powf(10.0f, preamp / 20.0f);
	//preamp = (( 1.0 + 0.0932471 * preamp + 0.00279033 * preamp*preamp ) / 2);

	return 0;
}
