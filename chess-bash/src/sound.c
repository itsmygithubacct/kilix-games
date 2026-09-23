#include "chess_bash.h"
#include "pcmmix_bank.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SR 44100
#define MAX_SFX_VARIANTS 6

typedef struct { int16_t *data; int len; } Sample;
static pcmmix_bank sound_bank;
static Sample music_tracks[MUSIC_COUNT];
static pcmmix mixer;
static bool mixer_started;
static uint32_t srng = 0x51a7c0deu;

static float frand_audio(void)
{
    srng ^= srng << 13;
    srng ^= srng >> 17;
    srng ^= srng << 5;
    return (srng >> 8) * (1.0f / 16777216.0f);
}

static float noise_audio(void) { return frand_audio() * 2.0f - 1.0f; }

static void bake(int id, const float *src, int n, float peak)
{
    float maxv = 1e-6f;
    for (int i = 0; i < n; i++)
        if (fabsf(src[i]) > maxv) maxv = fabsf(src[i]);
    int16_t *out = malloc((size_t)n * sizeof *out);
    if (!out) return;
    float gain = peak / maxv;
    for (int i = 0; i < n; i++) {
        float v = clampf(src[i] * gain, -1.0f, 1.0f);
        int fi = 64, fo = 360;
        if (i < fi) v *= (float)i / fi;
        if (n - i < fo) v *= (float)(n - i) / fo;
        out[i] = (int16_t)(v * 32767.0f);
    }
    pcmmix_bank_clear_cue(&sound_bank, (uint32_t)id);
    if (!pcmmix_bank_take(&sound_bank, (uint32_t)id, 0u, out,
                          (size_t)n, 1.0f, 1.0f))
        free(out);
}

static void gen_beep(int id, float f0, float f1, float dur, float peak)
{
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = f0 + (f1 - f0) * (t / dur);
        ph += 6.2831853f * f / SR;
        float env = expf(-t / (dur * 0.42f));
        s[i] = sinf(ph) * env;
    }
    bake(id, s, n, peak);
    free(s);
}

static void gen_move(void)
{
    int n = (int)(0.115f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float thump = expf(-t / 0.026f);
        float scrape = expf(-t / 0.052f);
        float ph = 6.2831853f * 92.0f * t;
        s[i] = sinf(ph) * 0.72f * thump + noise_audio() * 0.30f * scrape;
    }
    bake(SND_MOVE, s, n, 0.34f);
    free(s);
}

static void gen_capture(void)
{
    /* sword clash, not a bell: noise snap + fast inharmonic steel partials */
    static const float pf[6] = { 2210, 2967, 3743, 4638, 5512, 1493 };
    static const float ptau[6] = { 0.085f, 0.070f, 0.060f, 0.048f, 0.038f, 0.055f };
    static const float pa[6] = { 0.30f, 0.34f, 0.26f, 0.20f, 0.15f, 0.14f };
    static const float pph[6] = { 0.3f, 1.1f, 2.2f, 3.9f, 5.1f, 0.7f };
    int n = (int)(0.34f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float v = noise_audio() * (0.9f * expf(-t / 0.030f) +
                                   0.5f * expf(-t / 0.0045f));
        for (int k = 0; k < 6; k++)
            v += sinf(6.2831853f * pf[k] * t + pph[k]) * pa[k] * expf(-t / ptau[k]);
        v += sinf(6.2831853f * 168.0f * t) * 0.32f * expf(-t / 0.045f);
        s[i] = v;
    }
    bake(SND_CAPTURE, s, n, 0.58f);
    free(s);
}

static void gen_fall(void)
{
    int n = (int)(0.36f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float low = expf(-t / 0.105f);
        float dirt = expf(-t / 0.130f);
        s[i] = sinf(6.2831853f * 72.0f * t) * 0.82f * low +
               noise_audio() * 0.46f * dirt;
    }
    bake(SND_FALL, s, n, 0.50f);
    free(s);
}

static void add_trumpet_note(float *s, int n, float start, float dur, float freq, float amp)
{
    int a = (int)(start * SR);
    int b = (int)((start + dur) * SR);
    if (a < 0) a = 0;
    if (b > n) b = n;
    float ph = 0;
    for (int i = a; i < b; i++) {
        float t = (float)(i - a) / SR;
        float env = fminf(t / 0.035f, 1.0f) * expf(-t / (dur * 0.95f));
        ph += 6.2831853f * freq / SR;
        float brass = sinf(ph) + 0.48f * sinf(ph * 2.0f) + 0.22f * sinf(ph * 3.0f);
        float bite = brass > 0 ? 1.0f : -0.65f;
        s[i] += tanhf((brass + bite * 0.18f) * 1.7f) * env * amp;
    }
}

static void gen_trumpet(int id, bool victory)
{
    float dur = victory ? 1.40f : 0.82f;
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    if (victory) {
        add_trumpet_note(s, n, 0.00f, 0.28f, 392.00f, 0.75f);
        add_trumpet_note(s, n, 0.24f, 0.28f, 493.88f, 0.75f);
        add_trumpet_note(s, n, 0.48f, 0.36f, 587.33f, 0.82f);
        add_trumpet_note(s, n, 0.78f, 0.55f, 783.99f, 0.92f);
    } else {
        add_trumpet_note(s, n, 0.00f, 0.24f, 392.00f, 0.72f);
        add_trumpet_note(s, n, 0.21f, 0.24f, 523.25f, 0.78f);
        add_trumpet_note(s, n, 0.42f, 0.34f, 659.25f, 0.86f);
    }
    bake(id, s, n, victory ? 0.62f : 0.50f);
    free(s);
}

static void gen_check(void)
{
    /* short two-note danger sting: brass hit dropping a tritone */
    float dur = 0.34f;
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    add_trumpet_note(s, n, 0.00f, 0.14f, 830.61f, 0.85f);
    add_trumpet_note(s, n, 0.12f, 0.20f, 587.33f, 0.90f);
    bake(SND_CHECK, s, n, 0.48f);
    free(s);
}

static void synth_all(void)
{
    gen_beep(SND_SELECT, 760.0f, 1180.0f, 0.075f, 0.18f);
    gen_move();
    gen_capture();
    gen_fall();
    gen_trumpet(SND_START_TRUMPET, false);
    gen_trumpet(SND_WIN_TRUMPET, true);
    gen_check();
}

static bool load_wav_into(Sample *dst, const char *path)
{
    char error[256];
    size_t frames = 0;
    int16_t *pcm = pcmmix_wav_load(path, &frames, error, sizeof error);
    if (!pcm || frames > INT_MAX) {
        pcmmix_wav_free(pcm);
        return false;
    }
    free(dst->data);
    dst->data = pcm;
    dst->len = (int)frames;
    return true;
}

static void load_sfx_bank(int id, const char *const *paths, int count)
{
    char error[256];

    if (id < 0 || id >= SOUND_COUNT || !paths || count < 1) return;
    if (count > MAX_SFX_VARIANTS) count = MAX_SFX_VARIANTS;
    for (int variant = 0; variant < count; variant++)
        if (!pcmmix_bank_load_wav(&sound_bank, (uint32_t)id,
                                  (uint32_t)variant,
                                  asset_path(paths[variant]), 1.0f, 1.0f,
                                  error, sizeof error))
            break;
}

static void load_sfx_assets(void)
{
    static const char *const select[] = {
        "sfx/select.wav", "sfx/select_v02.wav", "sfx/select_v03.wav"
    };
    static const char *const move[] = {
        "sfx/move_step.wav", "sfx/move_step_v02.wav", "sfx/move_step_v03.wav",
        "sfx/move_step_v04.wav", "sfx/move_step_v05.wav", "sfx/move_step_v06.wav"
    };
    static const char *const capture[] = {
        "sfx/capture_clank.wav", "sfx/capture_clank_v02.wav",
        "sfx/capture_clank_v03.wav", "sfx/capture_clank_v04.wav",
        "sfx/capture_clank_v05.wav"
    };
    static const char *const fall[] = {
        "sfx/fall_thud.wav", "sfx/fall_thud_v02.wav",
        "sfx/fall_thud_v03.wav", "sfx/fall_thud_v04.wav"
    };
    static const char *const start[] = {
        "sfx/start_trumpet.wav", "sfx/start_trumpet_v02.wav"
    };
    static const char *const win[] = {
        "sfx/win_trumpet.wav", "sfx/win_trumpet_v02.wav"
    };
    static const char *const check[] = {
        "sfx/check.wav", "sfx/check_v02.wav", "sfx/check_v03.wav"
    };

    load_sfx_bank(SND_SELECT, select, (int)(sizeof select / sizeof select[0]));
    load_sfx_bank(SND_MOVE, move, (int)(sizeof move / sizeof move[0]));
    load_sfx_bank(SND_CAPTURE, capture, (int)(sizeof capture / sizeof capture[0]));
    load_sfx_bank(SND_FALL, fall, (int)(sizeof fall / sizeof fall[0]));
    load_sfx_bank(SND_START_TRUMPET, start, (int)(sizeof start / sizeof start[0]));
    load_sfx_bank(SND_WIN_TRUMPET, win, (int)(sizeof win / sizeof win[0]));
    load_sfx_bank(SND_CHECK, check, (int)(sizeof check / sizeof check[0]));
}

static void load_music_assets(void)
{
    load_wav_into(&music_tracks[MUS_THINKING], asset_path("music/thinking_loop.wav"));
    load_wav_into(&music_tracks[MUS_BATTLE], asset_path("music/battle_loop.wav"));
    load_wav_into(&music_tracks[MUS_VICTORY], asset_path("music/victory_fanfare.wav"));
}

/* ---------- public API ---------- */
bool sound_init(void)
{
    pcmmix_options options;

    (void)pcmmix_bank_init(&sound_bank, SOUND_COUNT, 0x51a7c0deu);
    synth_all();
    load_sfx_assets();
    load_music_assets();
    pcmmix_options_init(&options);
    options.max_voices = 16;
    if (!pcmmix_start(&mixer, &options)) return false;
    mixer_started = true;
    return true;
}

void sound_shutdown(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    pcmmix_bank_clear(&sound_bank);
    for (int i = 0; i < MUSIC_COUNT; i++) {
        pcmmix_wav_free(music_tracks[i].data);
        music_tracks[i] = (Sample){0};
    }
}

void sound_music_play(int id, float vol, bool loop)
{
    if (!mixer_started || id < 0 || id >= MUSIC_COUNT ||
        !music_tracks[id].data)
        return;
    pcmmix_sample track = {
        music_tracks[id].data, (size_t)music_tracks[id].len
    };
    pcmmix_music_play(&mixer, &track, clampf(vol, 0, 1.2f), loop);
}

void sound_music_stop(float fade_seconds)
{
    if (mixer_started) pcmmix_music_stop(&mixer, fade_seconds);
}

int sound_music_current(void)
{
    if (!mixer_started) return -1;
    const int16_t *current = pcmmix_music_current(&mixer);
    for (int id = 0; id < MUSIC_COUNT; id++)
        if (music_tracks[id].data == current) return id;
    return -1;
}

void sound_play(int id, float vol, float pitch)
{
    if (!mixer_started || id < 0 || id >= SOUND_COUNT) return;
    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)id,
                           clampf(vol, 0, 1.5f),
                           pitch <= 0 ? 1.0f : pitch);
}
