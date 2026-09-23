/* sound.c - strict WAV loading with graceful pcm-mixer playback. */
#include "sound.h"

#include "pcmmix_bank.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum { SOUND_RATE = 44100, SOUND_MAX_FRAMES = SOUND_RATE };

static pcmmix mixer;
static pcmmix_bank sound_bank;
static bool logical_enabled = true;
static bool loaded;
static bool started;
static bool offline_mode;
static bool hard_disabled;
static uint64_t trace_count;

static bool string_copy(char *dst, size_t size, const char *src)
{
    int n;
    if (dst == NULL || size == 0 || src == NULL) return false;
    n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size;
}

static bool path_join(char *dst, size_t size, const char *a, const char *b)
{
    size_t length;
    int n;
    if (dst == NULL || a == NULL || b == NULL) return false;
    length = strlen(a);
    n = snprintf(dst, size, "%s%s%s", a,
                 length > 0 && a[length - 1] == '/' ? "" : "/", b);
    return n >= 0 && (size_t)n < size;
}

static bool dirname_of(const char *path, char *dst, size_t size)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    size_t length;
    if (path == NULL || path[0] == '\0') return false;
    if (slash == NULL) return string_copy(dst, size, ".");
    if (slash == path) return string_copy(dst, size, "/");
    length = (size_t)(slash - path);
    if (length + 1u > size) return false;
    memcpy(dst, path, length);
    dst[length] = '\0';
    return true;
}

static bool executable_path(const char *argv0, char *dst, size_t size)
{
    char resolved[PATH_MAX];
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", resolved, sizeof resolved - 1u);
    if (n > 0 && (size_t)n < sizeof resolved) {
        resolved[n] = '\0';
        return string_copy(dst, size, resolved);
    }
#endif
    if (argv0 != NULL && strchr(argv0, '/') != NULL &&
        realpath(argv0, resolved) != NULL)
        return string_copy(dst, size, resolved);
    return false;
}

static bool readable_wav(const char *directory)
{
    char path[PATH_MAX];
    return path_join(path, sizeof path, directory, "light-switch.wav") &&
           access(path, R_OK) == 0;
}

static bool resolve_directory(const char *argv0, char *dst, size_t size)
{
    const char *override = getenv("KILIX_LIGHTS_ASSETS");
    char executable[PATH_MAX];
    char directory[PATH_MAX];
    char candidate[PATH_MAX];

    if (override != NULL && override[0] != '\0') {
        if (readable_wav(override)) return string_copy(dst, size, override);
        if (path_join(candidate, sizeof candidate, override, "sfx") &&
            readable_wav(candidate))
            return string_copy(dst, size, candidate);
    }
    if (executable_path(argv0, executable, sizeof executable) &&
        dirname_of(executable, directory, sizeof directory) &&
        path_join(candidate, sizeof candidate, directory, "../assets/sfx") &&
        readable_wav(candidate))
        return string_copy(dst, size, candidate);
    if (readable_wav("assets/sfx"))
        return string_copy(dst, size, "assets/sfx");
    return string_copy(dst, size, "assets/sfx");
}

static bool load_asset(const char *argv0, bool verbose, bool retain)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char error[256];
    int16_t *frames;
    size_t frame_count = 0;
    bool nonzero = false;
    bool clipped = false;

    if (!resolve_directory(argv0, directory, sizeof directory) ||
        !path_join(path, sizeof path, directory, "light-switch.wav"))
        return false;
    frames = pcmmix_wav_load(path, &frame_count, error, sizeof error);
    if (frames == NULL) {
        if (verbose) fprintf(stderr, "audio: %s\n", error);
        return false;
    }
    if (frame_count < 1000u || frame_count > SOUND_MAX_FRAMES) {
        if (verbose)
            fprintf(stderr, "audio: invalid switch duration (%zu frames)\n",
                    frame_count);
        pcmmix_wav_free(frames);
        return false;
    }
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i] != 0) nonzero = true;
        if (frames[i] == INT16_MIN || frames[i] == INT16_MAX) clipped = true;
    }
    if (!nonzero || clipped) {
        if (verbose) fprintf(stderr, "audio: switch cue is silent or clipped\n");
        pcmmix_wav_free(frames);
        return false;
    }
    if (retain) {
        pcmmix_bank_clear_cue(&sound_bank, 0u);
        if (!pcmmix_bank_take(&sound_bank, 0u, 0u, frames, frame_count,
                              1.0f, 1.0f)) {
            pcmmix_wav_free(frames);
            return false;
        }
    } else {
        pcmmix_wav_free(frames);
    }
    if (verbose)
        printf("audio: light-switch.wav %zu frames %.3fs OK\n", frame_count,
               (double)frame_count / SOUND_RATE);
    return true;
}

static bool env_disables_audio(void)
{
    const char *audio = getenv("KILIX_LIGHTS_AUDIO");
    const char *no_audio = getenv("KILIX_LIGHTS_NO_AUDIO");
    bool audio_off = audio != NULL &&
        (strcmp(audio, "0") == 0 || strcasecmp(audio, "off") == 0 ||
         strcasecmp(audio, "false") == 0 || strcasecmp(audio, "no") == 0);
    bool explicit_no = no_audio != NULL && no_audio[0] != '\0' &&
        strcmp(no_audio, "0") != 0 && strcasecmp(no_audio, "off") != 0 &&
        strcasecmp(no_audio, "false") != 0;
    return audio_off || explicit_no;
}

bool sound_init(const char *argv0, bool offline)
{
    pcmmix_options options;
    sound_shutdown();
    (void)pcmmix_bank_init(&sound_bank, 1u, 0x11a675u);
    hard_disabled = !offline && env_disables_audio();
    if (hard_disabled) logical_enabled = false;
    loaded = load_asset(argv0, false, true);
    if (!loaded) return false;
    if (hard_disabled) return true;
    pcmmix_options_init(&options);
    options.offline = offline;
    options.latency_ms = 18;
    started = pcmmix_start(&mixer, &options);
    offline_mode = started && offline;
    if (started) pcmmix_set_enabled(&mixer, logical_enabled);
    return offline ? started : true;
}

void sound_shutdown(void)
{
    if (started) pcmmix_stop(&mixer);
    started = false;
    offline_mode = false;
    hard_disabled = false;
    pcmmix_bank_clear(&sound_bank);
    loaded = false;
}

void sound_play_switch(bool ended_on)
{
    trace_count++;
    if (!logical_enabled || !started || !pcmmix_is_running(&mixer)) return;
    (void)pcmmix_bank_play(&mixer, &sound_bank, 0u, 0.78f,
                           ended_on ? 1.04f : 0.94f);
}

void sound_set_enabled(bool enabled)
{
    logical_enabled = enabled && !hard_disabled;
    if (started) pcmmix_set_enabled(&mixer, logical_enabled);
}

bool sound_is_enabled(void)
{
    return logical_enabled;
}

bool sound_validate(const char *argv0, bool verbose)
{
    return load_asset(argv0, verbose, false);
}

void sound_reset_trace(void)
{
    trace_count = 0u;
}

uint64_t sound_trace_count(void)
{
    return trace_count;
}

bool sound_mix_offline(int16_t *dst, size_t frames)
{
    if (dst == NULL || frames == 0) return false;
    if (!started || !offline_mode) {
        memset(dst, 0, frames * sizeof *dst);
        return false;
    }
    pcmmix_mix_block(&mixer, dst, frames);
    return true;
}

bool sound_asset_loaded(void)
{
    return loaded;
}

bool sound_mixer_running(void)
{
    return started && pcmmix_is_running(&mixer);
}
