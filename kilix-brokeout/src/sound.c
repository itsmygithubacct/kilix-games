/* Banked PCM audio with a procedural fallback. The effect bank is synthesized
 * at startup, then each valid reviewed WAV replaces its corresponding sample.
 * Transport and voice mixing live in the vendored pcm-mixer library. */
#include "kitty_brokeout.h"
#include "pcmmix_bank.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SR 44100
#define MAX_VOICES 24

static pcmmix_bank sound_bank;
static pcmmix mixer;
static bool mixerStarted = false;
static bool enabled = true;
static uint32_t srng = 0x5f3759dfu;

static float srandf01(void)
{
    srng ^= srng << 13;
    srng ^= srng >> 17;
    srng ^= srng << 5;
    return (srng >> 8) * (1.0f / 16777216.0f);
}

static float snoise(void) { return srandf01() * 2.0f - 1.0f; }

static float lpk(float c)
{
    return 1.0f - expf(-6.2831853f * c / SR);
}

static void bake(int id, const float *src, int n, float peak, bool fade)
{
    if (id < 0 || id >= SOUND_COUNT || n <= 0) return;
    float maxv = 1e-6f;
    for (int i = 0; i < n; i++)
        if (fabsf(src[i]) > maxv) maxv = fabsf(src[i]);

    int16_t *out = malloc((size_t)n * sizeof *out);
    if (!out) return;
    float gain = peak / maxv;
    for (int i = 0; i < n; i++) {
        float v = src[i] * gain;
        if (fade) {
            int fi = 64, fo = 420;
            if (i < fi) v *= (float)i / fi;
            if (n - i < fo) v *= (float)(n - i) / fo;
        }
        v = clampf(v, -1.0f, 1.0f);
        out[i] = (int16_t)(v * 32767.0f);
    }
    pcmmix_bank_clear_cue(&sound_bank, (uint32_t)id);
    if (!pcmmix_bank_take(&sound_bank, (uint32_t)id, 0u, out,
                          (size_t)n, 1.0f, 1.0f))
        free(out);
}

static void gen_sweep(int id, float f0, float f1, float dur, float peak)
{
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float u = t / dur;
        float f = f0 + (f1 - f0) * u;
        ph += 6.2831853f * f / SR;
        float env = expf(-t / (dur * 0.42f));
        s[i] = (sinf(ph) * 0.78f + sinf(ph * 2.01f) * 0.18f) * env;
    }
    bake(id, s, n, peak, true);
    free(s);
}

static void gen_metal(void)
{
    int n = (int)(0.34f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph1 = 0, ph2 = 0, ph3 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        ph1 += 6.2831853f * 660.0f / SR;
        ph2 += 6.2831853f * 927.0f / SR;
        ph3 += 6.2831853f * 1320.0f / SR;
        float env = expf(-t / 0.11f);
        s[i] = (sinf(ph1) * 0.55f + sinf(ph2) * 0.42f + sinf(ph3) * 0.25f) * env;
    }
    bake(SND_METAL, s, n, 0.34f, true);
    free(s);
}

static void gen_explode(void)
{
    int n = (int)(0.82f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float lp = 0, sub = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float cut = 4800.0f * expf(-t * 5.6f) + 120.0f;
        lp += lpk(cut) * (snoise() - lp);
        sub += 6.2831853f * (118.0f * expf(-t * 7.5f) + 35.0f) / SR;
        float env = expf(-t / 0.20f);
        s[i] = tanhf((lp * 1.55f + sinf(sub) * 0.9f) * env * 2.0f);
    }
    bake(SND_EXPLODE, s, n, 0.62f, true);
    free(s);
}

static void gen_powerup(void)
{
    int n = (int)(0.48f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    static const float notes[4] = { 523.25f, 659.25f, 783.99f, 1046.50f };
    for (int k = 0; k < 4; k++) {
        int start = (int)(k * 0.065f * SR);
        for (int i = 0; i < (int)(0.18f * SR) && start + i < n; i++) {
            float t = (float)i / SR;
            s[start + i] += sinf(6.2831853f * notes[k] * t) * expf(-t / 0.12f);
        }
    }
    bake(SND_POWERUP, s, n, 0.32f, true);
    free(s);
}

static void gen_clear(void)
{
    int n = (int)(0.86f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    static const float notes[5] = { 392.0f, 523.25f, 659.25f, 783.99f, 1046.50f };
    for (int k = 0; k < 5; k++) {
        int start = (int)(k * 0.105f * SR);
        for (int i = 0; i < (int)(0.30f * SR) && start + i < n; i++) {
            float t = (float)i / SR;
            float ph = 6.2831853f * notes[k] * t;
            s[start + i] += (sinf(ph) * 0.82f + sinf(ph * 2.0f) * 0.12f) * expf(-t / 0.22f);
        }
    }
    bake(SND_CLEAR, s, n, 0.36f, true);
    free(s);
}

static void gen_lose(void)
{
    int n = (int)(0.48f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 260.0f * expf(-t * 2.4f) + 75.0f;
        ph += 6.2831853f * f / SR;
        s[i] = sinf(ph) * expf(-t / 0.28f);
    }
    bake(SND_LOSE, s, n, 0.31f, true);
    free(s);
}

static void synth_all(void)
{
    gen_sweep(SND_PADDLE, 360.0f, 520.0f, 0.075f, 0.24f);
    gen_sweep(SND_BRICK, 760.0f, 1180.0f, 0.085f, 0.25f);
    gen_metal();
    gen_explode();
    gen_powerup();
    gen_sweep(SND_LAUNCH, 430.0f, 880.0f, 0.13f, 0.26f);
    gen_lose();
    gen_clear();
    gen_sweep(SND_MENU, 560.0f, 720.0f, 0.07f, 0.17f);
}

static const char *const sound_files[SOUND_COUNT] = {
    [SND_PADDLE] = "sfx/paddle.wav",
    [SND_BRICK] = "sfx/brick.wav",
    [SND_METAL] = "sfx/metal.wav",
    [SND_EXPLODE] = "sfx/explode.wav",
    [SND_POWERUP] = "sfx/powerup.wav",
    [SND_LAUNCH] = "sfx/launch.wav",
    [SND_LOSE] = "sfx/lose.wav",
    [SND_CLEAR] = "sfx/clear.wav",
    [SND_MENU] = "sfx/menu.wav",
};

static char sound_asset_root[512] = "assets";

static void sound_asset_paths_init(void)
{
    const char *override = getenv("KITTY_BROKEOUT_ASSETS");
    char executable[400];
    char candidate[512];
    char *slash;
    ssize_t length;

    if (override && *override) {
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", override);
        return;
    }
    length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(candidate, sizeof candidate, "%s/assets", executable);
    if (access(candidate, F_OK) == 0) {
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", candidate);
        return;
    }
    snprintf(candidate, sizeof candidate,
             "%s/../share/kitty-brokeout/assets", executable);
    if (access(candidate, F_OK) == 0)
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", candidate);
}

static void load_external_sounds(void)
{
    sound_asset_paths_init();
    for (int id = 0; id < SOUND_COUNT; id++) {
        char full[768];
        char err[128];
        if (!sound_files[id]) continue;
        if (snprintf(full, sizeof full, "%s/%s", sound_asset_root,
                     sound_files[id]) >= (int)sizeof full)
            continue;
        (void)pcmmix_bank_load_wav(&sound_bank, (uint32_t)id, 0u,
                                   full, 1.0f, 1.0f,
                                   err, sizeof err);
    }
}

bool sound_init(void)
{
    pcmmix_options options;

    (void)pcmmix_bank_init(&sound_bank, SOUND_COUNT, 0x5f3759dfu);
    synth_all();
    load_external_sounds();
    pcmmix_options_init(&options);
    options.sample_rate = SR;
    options.max_voices = MAX_VOICES;
    if (!pcmmix_start(&mixer, &options)) return false;
    mixerStarted = true;
    pcmmix_set_enabled(&mixer, enabled);
    return true;
}

void sound_shutdown(void)
{
    if (mixerStarted) {
        pcmmix_stop(&mixer);
        mixerStarted = false;
    }
    pcmmix_bank_clear(&sound_bank);
}

void sound_set_enabled(bool on)
{
    enabled = on;
    if (mixerStarted) pcmmix_set_enabled(&mixer, on);
}

bool sound_is_enabled(void) { return enabled; }

void sound_play(int id, float vol, float pitch)
{
    if (id < 0 || id >= SOUND_COUNT || !mixerStarted) return;
    /* the original mixer soft-clipped with tanh(mix * 0.85); pcm-mixer's
     * master bus is tanh(mix), so fold the 0.85 into the voice gain to
     * keep the same loudness curve */
    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)id,
                           clampf(vol, 0, 1.5f) * 0.85f, pitch);
}

void sound_loop(int id, bool on, float vol, float pitch)
{
    /* the game never holds looped effects; kept for API compatibility */
    (void)id;
    (void)on;
    (void)vol;
    (void)pitch;
}
