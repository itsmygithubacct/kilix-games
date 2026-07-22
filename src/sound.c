/* Deterministic procedural audio; there are no runtime sound assets. */
#include "kilix_jpak.h"
#include "pcmmix_bank.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define TAU 6.2831853071795864769f

static pcmmix_bank sound_bank;
static pcmmix mixer;
static bool started;
static bool enabled = true;
static int jet_voice = -1;
static uint32_t noise_state = 0x5eedc0deu;

static float noise_sample(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return ((noise_state >> 8) * (1.0f / 8388608.0f)) - 1.0f;
}

static void bake(int id, const float *input, size_t count, float peak, bool fade)
{
    float maximum = 1e-6f;
    for (size_t i = 0; i < count; i++) {
        float a = fabsf(input[i]);
        if (a > maximum) maximum = a;
    }
    int16_t *data = malloc(count * sizeof *data);
    if (!data) return;
    float gain = peak / maximum;
    size_t edge = SAMPLE_RATE / 200; /* five milliseconds */
    for (size_t i = 0; i < count; i++) {
        float value = input[i] * gain;
        if (fade && i < edge) value *= (float)i / (float)edge;
        if (fade && count - i < edge) value *= (float)(count - i) / (float)edge;
        value = clampf(value, -1.0f, 1.0f);
        data[i] = (int16_t)lrintf(value * 32767.0f);
    }
    pcmmix_bank_clear_cue(&sound_bank, (uint32_t)id);
    if (!pcmmix_bank_take(&sound_bank, (uint32_t)id, 0u, data, count,
                          1.0f, 1.0f))
        free(data);
}

static void synth_tone(int id, float duration, float f0, float f1,
                       float decay, float peak, int waveform)
{
    size_t count = (size_t)(duration * SAMPLE_RATE);
    float *buffer = calloc(count, sizeof *buffer);
    if (!buffer) return;
    float phase = 0, filter = 0;
    for (size_t i = 0; i < count; i++) {
        float t = (float)i / SAMPLE_RATE;
        float u = t / duration;
        float frequency = f0 + (f1 - f0) * u;
        phase += TAU * frequency / SAMPLE_RATE;
        float wave;
        if (waveform == 1) wave = asinf(sinf(phase)) * (2.0f / 3.14159265f);
        else if (waveform == 2) wave = sinf(phase) > 0 ? 1.0f : -1.0f;
        else if (waveform == 3) {
            filter += .16f * (noise_sample() - filter);
            wave = filter * .78f + sinf(phase) * .35f;
        } else wave = sinf(phase) + .18f * sinf(phase * 2.01f);
        float envelope = decay > 0 ? expf(-t / decay) : 1.0f;
        buffer[i] = wave * envelope;
    }
    bake(id, buffer, count, peak, true);
    free(buffer);
}

static void synth_notes(int id, const float *notes, int note_count,
                        float spacing, float note_length, float peak)
{
    float duration = spacing * (note_count - 1) + note_length;
    size_t count = (size_t)(duration * SAMPLE_RATE);
    float *buffer = calloc(count, sizeof *buffer);
    if (!buffer) return;
    for (int n = 0; n < note_count; n++) {
        size_t start = (size_t)(n * spacing * SAMPLE_RATE);
        size_t length = (size_t)(note_length * SAMPLE_RATE);
        for (size_t i = 0; i < length && start + i < count; i++) {
            float t = (float)i / SAMPLE_RATE;
            float phase = TAU * notes[n] * t;
            float envelope = expf(-t / (note_length * .52f));
            buffer[start + i] += (sinf(phase) * .82f + sinf(phase * 2) * .12f) * envelope;
        }
    }
    bake(id, buffer, count, peak, true);
    free(buffer);
}

static void synth_jet(void)
{
    size_t count = SAMPLE_RATE / 3;
    float *buffer = calloc(count, sizeof *buffer);
    if (!buffer) return;
    float low = 0;
    noise_state = 0x7a11ce55u;
    for (size_t i = 0; i < count; i++) {
        float window = .5f - .5f * cosf(TAU * (float)i / (float)count);
        low += .055f * (noise_sample() - low);
        /* Exactly 24 cycles make both the value and slope continuous when the
         * one-third-second buffer wraps.  The raised-cosine noise layer also
         * meets at zero, preventing the old three-clicks-per-second seam. */
        float phase = TAU * 24.0f * (float)i / (float)count;
        buffer[i] = low * window * .82f + sinf(phase) * .28f;
    }
    bake(SFX_JET, buffer, count, .44f, false);
    free(buffer);
}

static void synth_hit(void)
{
    size_t count = (size_t)(.52f * SAMPLE_RATE);
    float *buffer = calloc(count, sizeof *buffer);
    if (!buffer) return;
    float low = 0, phase = 0;
    noise_state = 0xc0111deu;
    for (size_t i = 0; i < count; i++) {
        float t = (float)i / SAMPLE_RATE;
        low += (.22f * expf(-t * 7) + .015f) * (noise_sample() - low);
        phase += TAU * (96.0f * expf(-t * 6) + 34.0f) / SAMPLE_RATE;
        buffer[i] = tanhf((low * 1.6f + sinf(phase) * .75f) * expf(-t / .17f) * 2.0f);
    }
    bake(SFX_HIT, buffer, count, .67f, true);
    free(buffer);
}

static void synth_all(void)
{
    noise_state = 0x5eedc0deu;
    synth_tone(SFX_MENU, .075f, 580, 760, .06f, .24f, 1);
    synth_jet();
    { const float n[] = {523.25f, 783.99f, 1174.66f};
      synth_notes(SFX_CRYSTAL, n, 3, .045f, .17f, .34f); }
    synth_tone(SFX_PICKUP, .20f, 420, 1040, .14f, .30f, 1);
    synth_tone(SFX_PHASE, .23f, 170, 1320, .20f, .26f, 3);
    synth_tone(SFX_TELEPORT, .42f, 1320, 115, .33f, .37f, 0);
    synth_tone(SFX_SWITCH, .12f, 190, 390, .055f, .34f, 2);
    { const float n[] = {293.66f, 440.0f, 659.25f, 987.77f};
      synth_notes(SFX_EXIT, n, 4, .065f, .25f, .38f); }
    synth_hit();
    synth_tone(SFX_STUN, .26f, 1640, 260, .16f, .30f, 2);
    { const float n[] = {392.0f, 523.25f, 659.25f, 783.99f, 1046.5f};
      synth_notes(SFX_CLEAR, n, 5, .095f, .31f, .42f); }
    { const float n[] = {523.25f, 659.25f, 783.99f, 1046.5f};
      synth_notes(SFX_LIFE, n, 4, .10f, .34f, .42f); }
    synth_tone(SFX_GAMEOVER, .78f, 310, 58, .42f, .43f, 0);
    { const float n[] = {261.63f, 329.63f, 392.0f, 523.25f, 659.25f,
                         783.99f, 1046.5f, 1318.51f};
      synth_notes(SFX_VICTORY, n, 8, .13f, .48f, .48f); }
}

bool sound_init(void)
{
    if (pcmmix_bank_variant_count(&sound_bank, 0u) == 0u) {
        (void)pcmmix_bank_init(&sound_bank, SFX_COUNT, 0x5eedc0deu);
        synth_all();
    }
    pcmmix_options options;
    pcmmix_options_init(&options);
    options.sample_rate = SAMPLE_RATE;
    options.max_voices = 24;
    if (!pcmmix_start(&mixer, &options)) return false;
    started = true;
    pcmmix_set_enabled(&mixer, enabled);
    return true;
}

void sound_shutdown(void)
{
    if (started) pcmmix_stop(&mixer);
    started = false;
    jet_voice = -1;
    pcmmix_bank_clear(&sound_bank);
}

void sound_set_enabled(bool on)
{
    if (!on && started && jet_voice > 0)
        pcmmix_voice_stop(&mixer, jet_voice);
    enabled = on;
    if (started) pcmmix_set_enabled(&mixer, on);
    if (!on) jet_voice = -1;
}

bool sound_is_enabled(void)
{
    return enabled;
}

void sound_play(int id, float volume, float pitch)
{
    if (!started || !enabled || id < 0 || id >= SFX_COUNT || id == SFX_JET)
        return;
    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)id,
                           volume, pitch);
}

void sound_jet(bool active, float intensity)
{
    if (!started || !enabled) return;
    if (!active) {
        if (jet_voice > 0) pcmmix_voice_stop(&mixer, jet_voice);
        jet_voice = -1;
        return;
    }
    float volume = .10f + clampf(intensity, 0, 1) * .13f;
    if (jet_voice > 0 && pcmmix_voice_active(&mixer, jet_voice)) {
        pcmmix_voice_set(&mixer, jet_voice, volume, .94f + intensity * .16f);
        return;
    }
    jet_voice = pcmmix_bank_loop(&mixer, &sound_bank, SFX_JET,
                                 volume, .98f);
}
