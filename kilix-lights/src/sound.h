/* sound.h - one-cue, failure-safe light-switch audio. */
#ifndef KILIX_LIGHTS_SOUND_H
#define KILIX_LIGHTS_SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool sound_init(const char *argv0, bool offline);
void sound_shutdown(void);
void sound_play_switch(bool ended_on);
void sound_set_enabled(bool enabled);
bool sound_is_enabled(void);
bool sound_validate(const char *argv0, bool verbose);

void sound_reset_trace(void);
uint64_t sound_trace_count(void);
bool sound_mix_offline(int16_t *dst, size_t frames);
bool sound_asset_loaded(void);
bool sound_mixer_running(void);

#endif
