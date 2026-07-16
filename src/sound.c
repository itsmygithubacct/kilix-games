/* Banked PCM playback for the python-authored bank in assets/sfx/. The shared
 * pcm-mixer owns voices and sink transport. There is deliberately no
 * procedural fallback: a missing bank goes silent and reports the problem
 * instead of hiding a broken install behind substitute C-synth beeps. */
#include "kilix_pong.h"
#include "pcm_mixer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    int16_t *data;
    int length;
} Sample;

static Sample samples[SFX_COUNT][SFX_VARIANTS];
static uint8_t sample_counts[SFX_COUNT];
static uint8_t next_variant[SFX_COUNT];
static pcmmix mixer;
static bool mixer_started;
static bool enabled = true;
/* Two distinct facts, deliberately not conflated: bank_complete means all
   SFX_COUNT*SFX_VARIANTS production WAVs loaded, which is what --asset-check
   must attest; bank_playable means at least one did, which is all playback
   needs. Reporting a 1-of-21 bank as "loaded" would make the asset validator
   weaker than its name. */
static bool bank_complete;
static bool bank_playable;
static uint32_t rotate_rng = 0x9E3779B9u;

/* Filenames match tools/gen_sfx.py: variant 1 is the bare cue name, later
   variants take the _vNN suffix. Order matches the SFX_* enum. */
static const char *const CUE_NAMES[SFX_COUNT] = {
    "paddle", "wall", "score", "serve", "miss", "menu", "gameover"
};

const char *sound_sink_name(void)
{
    return mixer_started ? pcmmix_backend_name(&mixer) : NULL;
}
/* Attests a COMPLETE production bank; --asset-check relies on this. */
bool sound_bank_loaded(void) { return bank_complete; }
bool sound_is_enabled(void)
{
    return enabled && bank_playable && mixer_started &&
           pcmmix_is_enabled(&mixer);
}
void sound_set_enabled(bool on)
{
    enabled = on;
    if (mixer_started) pcmmix_set_enabled(&mixer, on);
}
static uint32_t rotate_random(void)
{
    rotate_rng ^= rotate_rng << 13;
    rotate_rng ^= rotate_rng >> 17;
    rotate_rng ^= rotate_rng << 5;
    return rotate_rng;
}

static void free_bank(void)
{
    for (int cue = 0; cue < SFX_COUNT; cue++) {
        for (int v = 0; v < SFX_VARIANTS; v++) {
            free(samples[cue][v].data);
            samples[cue][v] = (Sample){0};
        }
        sample_counts[cue] = 0;
    }
}

/* Builds "sfx/<cue>.wav" or "sfx/<cue>_vNN.wav" for asset_path(). */
static void variant_relpath(char *out, size_t len, const char *cue, int variant)
{
    if (variant == 0)
        snprintf(out, len, "sfx/%s.wav", cue);
    else
        snprintf(out, len, "sfx/%s_v%02d.wav", cue, variant + 1);
}

static bool load_bank(void)
{
    int loaded = 0, missing = 0;
    char rel[64], err[256];

    for (int cue = 0; cue < SFX_COUNT; cue++) {
        for (int v = 0; v < SFX_VARIANTS; v++) {
            variant_relpath(rel, sizeof rel, CUE_NAMES[cue], v);
            const char *path = asset_path(rel);
            size_t frames = 0;
            int16_t *data = pcmmix_wav_load(path, &frames, err, sizeof err);
            if (!data || frames > INT_MAX) {
                pcmmix_wav_free(data);
                if (missing < 3)   /* one line per problem, but do not spam 21 */
                    fprintf(stderr, "kilix-pong: sfx: %s\n", err);
                missing++;
                continue;
            }
            samples[cue][v].data = data;
            samples[cue][v].length = (int)frames;
            sample_counts[cue]++;
            loaded++;
        }
    }

    if (missing > 3)
        fprintf(stderr, "kilix-pong: sfx: ... and %d more\n", missing - 3);

    if (loaded == 0) {
        fprintf(stderr,
                "kilix-pong: no sound bank found; running silent.\n"
                "  Regenerate it with: python3 tools/gen_sfx.py\n"
                "  Or point KILIX_PONG_ASSETS at an install's assets directory.\n");
        return false;
    }
    bank_playable = true;
    bank_complete = (missing == 0);
    if (missing > 0)
        fprintf(stderr, "kilix-pong: sfx: INCOMPLETE bank -- %d of %d variants "
                        "missing; affected cues fall back to the variants "
                        "present. This is not a production bank.\n",
                missing, SFX_COUNT * SFX_VARIANTS);
    return true;
}

bool sound_init(void)
{
    pcmmix_options options;

    if (!load_bank()) return false;
    pcmmix_options_init(&options);
    options.max_voices = 16;
    options.latency_ms = 18;
    if (!pcmmix_start(&mixer, &options)) {
        fprintf(stderr, "kilix-pong: no audio sink (tried pacat, pw-play, "
                        "aplay, play); running silent\n");
        return false;
    }
    mixer_started = true;
    pcmmix_set_enabled(&mixer, enabled);
    return true;
}

void sound_shutdown(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    free_bank();
    bank_complete = false;
    bank_playable = false;
}

void sound_play(int id, float volume, float pitch)
{
    if (!mixer_started || id < 0 || id >= SFX_COUNT ||
        !enabled || !bank_playable)
        return;

    int count = sample_counts[id];
    if (count <= 0) return;
    int choice = next_variant[id];
    if (count > 2)
        choice = (choice + 1 +
                  (int)(rotate_random() % (uint32_t)(count - 1))) % count;
    else if (count == 2)
        choice ^= 1;
    next_variant[id] = (uint8_t)choice;

    Sample *sample = &samples[id][choice];
    if (!sample->data) {
        for (int variant = 0; variant < SFX_VARIANTS; variant++)
            if (samples[id][variant].data) {
                sample = &samples[id][variant];
                break;
            }
        if (!sample->data) return;
    }

    pcmmix_sample clip = {sample->data, (size_t)sample->length};
    (void)pcmmix_play(&mixer, &clip, clampf(volume, 0.0f, 2.0f),
                      clampf(pitch, 0.25f, 4.0f));
}
