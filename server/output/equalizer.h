#pragma once

void equalizer_init();
void equalizer_shutdown();
void equalizer_process_buffer(char *buf, size_t size, const sound_params &sp);
void equalizer_refresh();
bool equalizer_is_active();
void equalizer_set_active(bool active);
str  equalizer_current_eqname();
void equalizer_next();
void equalizer_prev();
