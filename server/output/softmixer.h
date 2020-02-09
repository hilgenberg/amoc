#pragma once

str  softmixer_name();

int  softmixer_get_value();
void softmixer_set_value(int val);

bool softmixer_is_active();
void softmixer_set_active(bool act);

bool softmixer_is_mono();
void softmixer_set_mono(bool mono);

void softmixer_process_buffer(char *buf, const size_t size, const sound_params &sp);
