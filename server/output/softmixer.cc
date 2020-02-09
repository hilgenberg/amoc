/*
 * MOC - music on console
 * Copyright (C) 2004-2008 Damian Pietras <daper@daper.net>
 *
 * Softmixer-extension Copyright (C) 2007-2008 Hendrik Iben <hiben@tzi.de>
 * Provides a software-mixer to regulate volume independent from
 * hardware.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "../audio.h"
#include "audio_conversion.h"
#include "softmixer.h"

void softmixer_set_value (int  v) { options::SoftmixerValue = CLAMP(0, v, 200); } 
void softmixer_set_active(bool v) { options::SoftmixerActive = v; } 
void softmixer_set_mono  (bool v) { options::SoftmixerMono = v; } 
int  softmixer_get_value() { return options::SoftmixerValue; } 
bool softmixer_is_active() { return options::SoftmixerActive; } 
bool softmixer_is_mono()   { return options::SoftmixerMono; }
str  softmixer_name()      { return options::SoftmixerActive ? "Soft" : "S.Off"; }

// promote int type to the next larger type
static inline constexpr uint16_t extend( uint8_t x) { return (uint16_t)x; }
static inline constexpr  int16_t extend(  int8_t x) { return ( int16_t)x; }
static inline constexpr uint32_t extend(uint16_t x) { return (uint32_t)x; }
static inline constexpr  int32_t extend( int16_t x) { return ( int32_t)x; }
static inline constexpr uint64_t extend(uint32_t x) { return (uint64_t)x; }
static inline constexpr  int64_t extend( int32_t x) { return ( int64_t)x; }
static inline constexpr    float extend(   float x) { return x; }

// promote uint type to the next larger signed type
static inline constexpr int16_t extend_s( uint8_t x) { return (int16_t)x; }
static inline constexpr int32_t extend_s(uint16_t x) { return (int32_t)x; }
static inline constexpr int64_t extend_s(uint32_t x) { return (int64_t)x; }

// scale buffer of unsigned
template<typename T>
static void process_u(T *buf, size_t N)
{
	constexpr auto M = extend_s(std::numeric_limits<T>::max());
	for (size_t i = 0; i < N; ++i)
	{
		auto k = extend_s(buf[i]);
		k -= M / 2;
		k  = k * options::SoftmixerValue / 100;
		k += M / 2;
		buf[i] = (T)CLAMP(0, k, M);
	}
}
// scale buffer of signed
template<typename T>
static void process_s(T *buf, size_t N)
{
	constexpr auto A = extend(std::numeric_limits<T>::min());
	constexpr auto B = extend(std::numeric_limits<T>::max());
	for (size_t i = 0; i < N; ++i)
	{
		auto k = extend(buf[i]);
		k = k * options::SoftmixerValue / 100;
		buf[i] = (T)CLAMP(A, k, B);
	}
}

template<typename T>
static void make_mono(T *buf, int channels, size_t samples)
{
	for (size_t i = 0; i < samples; i += channels)
	{
		auto k = extend((T)0);
		for (int c = 0; c < channels; ++c) k += buf[c];
		k /= channels;
		for (int c = 0; c < channels; ++c) *buf++ = (T)k;
	}
}

void softmixer_process_buffer(char *buf, size_t size, const sound_params &sp)
{
	const auto C = sp.channels;
	bool do_softmix = options::SoftmixerActive && options::SoftmixerValue != 100;
	bool do_monomix = options::SoftmixerMono   && C > 1;
	bool do_endian  = (sp.fmt & SFMT_MASK_ENDIANNESS != SFMT_NE);
	if(!do_softmix && !do_monomix) return;

	switch (sp.fmt & SFMT_MASK_FORMAT)
	{
		case SFMT_U8:
			if (do_softmix) process_u((uint8_t *)buf, size);
			if (do_monomix) make_mono((uint8_t *)buf, C, size);
			break;
		case SFMT_S8:
			if (do_softmix) process_s((int8_t *)buf, size);
			if (do_monomix) make_mono((int8_t *)buf, C, size);
			break;
		case SFMT_U16:
			size /= sizeof(uint16_t);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			if (do_softmix) process_u((uint16_t *)buf, size);
			if (do_monomix) make_mono((uint16_t *)buf, C, size);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			break;
		case SFMT_S16:
			size /= sizeof(int16_t);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			if (do_softmix) process_s((int16_t *)buf, size);
			if (do_monomix) make_mono((int16_t *)buf, C, size);
			if (do_endian)  audio_conv_bswap_16((int16_t *)buf, size);
			break;
		case SFMT_U32:
			size /= sizeof(uint32_t);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			if (do_softmix) process_u((uint32_t *)buf, size);
			if (do_monomix) make_mono((uint32_t *)buf, C, size);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			break;
		case SFMT_S32:
			size /= sizeof(int32_t);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			if (do_softmix) process_s((int32_t *)buf, size);
			if (do_monomix) make_mono((int32_t *)buf, C, size);
			if (do_endian)  audio_conv_bswap_32((int32_t *)buf, size);
			break;
		case SFMT_FLOAT:
			size /= sizeof(float);
			if (do_softmix) process_s((float *)buf, size);
			if (do_monomix) make_mono((float *)buf, C, size);
			break;
	}
}
