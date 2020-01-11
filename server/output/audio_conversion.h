#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <samplerate.h>

#include "../audio.h"

struct audio_conversion
{
	struct sound_params from;
	struct sound_params to;

	SRC_STATE *src_state;
	float *resample_buf;
	size_t resample_buf_nsamples; /* in samples ( sizeof(float) ) */
};

int audio_conv_new (struct audio_conversion *conv,
		const struct sound_params *from,
		const struct sound_params *to);
char *audio_conv (struct audio_conversion *conv,
		const char *buf, const size_t size, size_t *conv_len);
void audio_conv_destroy (struct audio_conversion *conv);

void audio_conv_bswap_16 (int16_t *buf, const size_t num);
void audio_conv_bswap_32 (int32_t *buf, const size_t num);
