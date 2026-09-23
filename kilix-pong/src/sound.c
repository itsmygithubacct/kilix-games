/* Banked PCM playback for the python-authored bank in assets/sfx/. The shared
 * pcm-mixer owns voices and sink transport. There is deliberately no
 * procedural fallback: a missing bank goes silent and reports the problem
 * instead of hiding a broken install behind substitute C-synth beeps. */
#include "kilix_pong.h"
#include "pcmmix_bank.h"

#include <stdio.h>
#include <string.h>


static pcmmix_bank sound_bank;
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
static void free_bank(void)
{
    pcmmix_bank_clear(&sound_bank);
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
            if (!pcmmix_bank_load_wav(&sound_bank, (uint32_t)cue,
                                      (uint32_t)v, path, 1.0f, 1.0f,
                                      err, sizeof err)) {
                if (missing < 3)   /* one line per problem, but do not spam 21 */
                    fprintf(stderr, "kilix-pong: sfx: %s\n", err);
                missing++;
                continue;
            }
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

    (void)pcmmix_bank_init(&sound_bank, SFX_COUNT, 0x9e3779b9u);
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

    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)id,
                           clampf(volume, 0.0f, 2.0f),
                           clampf(pitch, 0.25f, 4.0f));
}
