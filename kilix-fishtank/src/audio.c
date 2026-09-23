/* Optional ambience plus edge-triggered game events through the PCM mixer. */
#include "fishtank.h"
#include "pcmmix_bank.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const event_files[AUDIO_EVENT_COUNT] = {
    [AUDIO_FEED_SPLASH] = "events/feed_splash.wav",
    [AUDIO_FISH_BITE] = "events/fish_bite.wav",
    [AUDIO_SHARK_BITE] = "events/shark_bite.wav",
    [AUDIO_FRENZY] = "events/frenzy.wav",
    [AUDIO_CATCH_SPLASH] = "events/catch_splash.wav",
    [AUDIO_HOOK_BITE] = "events/hook_bite.wav",
    [AUDIO_HOOK_SET] = "events/hook_set.wav",
    [AUDIO_FISH_ESCAPE] = "events/fish_escape.wav",
    [AUDIO_LINE_SNAP] = "events/line_snap.wav",
    [AUDIO_SHARK_ALERT] = "events/shark_alert.wav",
};

static pcmmix mixer;
static bool mixer_started;
static pcmmix_bank sound_bank;
static char audio_root[1400] = "assets/audio";

enum { AUDIO_AMBIENCE = AUDIO_EVENT_COUNT, AUDIO_CUE_COUNT };

static bool env_is_off(const char *value)
{
    return value && (!strcmp(value, "0") || !strcmp(value, "off") ||
                     !strcmp(value, "false") || !strcmp(value, "none"));
}

static bool audio_disabled(void)
{
    return getenv("KILIX_FISHTANK_NO_AUDIO") ||
           env_is_off(getenv("KILIX_FISHTANK_AUDIO"));
}

static bool readable_file(const char *path)
{
    return path && *path && access(path, R_OK) == 0;
}

static bool readable_directory(const char *path)
{
    return path && *path && access(path, R_OK) == 0;
}

static bool executable_dir(const char *argv0, char *out, size_t out_size)
{
    char *slash;
    if (!argv0 || !strchr(argv0, '/')) return false;
    (void)snprintf(out, out_size, "%s", argv0);
    slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static void find_audio_root(const char *argv0)
{
    const char *override = getenv("KILIX_FISHTANK_ASSETS");
    char directory[1024];
    char candidate[1400];

    if (override && *override) {
        (void)snprintf(audio_root, sizeof audio_root, "%s/audio", override);
        return;
    }
    if (readable_directory("assets/audio")) return;
    if (!executable_dir(argv0, directory, sizeof directory)) return;
    (void)snprintf(candidate, sizeof candidate, "%s/assets/audio", directory);
    if (readable_directory(candidate)) {
        (void)snprintf(audio_root, sizeof audio_root, "%s", candidate);
        return;
    }
    (void)snprintf(candidate, sizeof candidate,
                   "%s/../share/kilix-fishtank/assets/audio", directory);
    if (readable_directory(candidate))
        (void)snprintf(audio_root, sizeof audio_root, "%s", candidate);
}

static bool load_file(uint32_t cue, const char *path, bool report)
{
    char error[256];
    if (pcmmix_bank_load_wav(&sound_bank, cue, 0u, path, 1.0f, 1.0f,
                             error, sizeof error))
        return true;
    if (report)
        (void)fprintf(stderr, "invalid or missing sound: %s (%s)\n", path,
                      error);
    return false;
}

static bool load_bank(const char *argv0, bool report)
{
    const char *explicit_path = getenv("KILIX_FISHTANK_AUDIO");
    char path[1800];
    bool loaded = true;

    pcmmix_bank_clear(&sound_bank);
    (void)pcmmix_bank_init(&sound_bank, AUDIO_CUE_COUNT, 0xf157a4b1u);
    find_audio_root(argv0);
    if (explicit_path && *explicit_path && !env_is_off(explicit_path) &&
        strstr(explicit_path, ".wav"))
        (void)snprintf(path, sizeof path, "%s", explicit_path);
    else
        (void)snprintf(path, sizeof path, "%s/fishtank_ambience.wav",
                       audio_root);
    if (!readable_file(path) || !load_file(AUDIO_AMBIENCE, path, report))
        loaded = false;
    for (int id = 0; id < AUDIO_EVENT_COUNT; ++id) {
        (void)snprintf(path, sizeof path, "%s/%s", audio_root,
                       event_files[id]);
        if (!load_file((uint32_t)id, path, report)) loaded = false;
    }
    return loaded;
}

static void free_bank(void)
{
    pcmmix_bank_clear(&sound_bank);
}

static float audio_volume(void)
{
    const char *value = getenv("KILIX_FISHTANK_AUDIO_VOLUME");
    int volume = value && *value ? atoi(value) : 18;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume / 100.0f;
}

void audio_start(const char *argv0)
{
    pcmmix_options options;
    if (mixer_started || audio_disabled()) return;
    (void)load_bank(argv0, false);
    if (pcmmix_bank_variant_count(&sound_bank, AUDIO_AMBIENCE) == 0u) {
        free_bank();
        return;
    }
    pcmmix_options_init(&options);
    options.max_voices = 24;
    if (!pcmmix_start(&mixer, &options)) {
        free_bank();
        return;
    }
    mixer_started = true;
    (void)pcmmix_bank_loop(&mixer, &sound_bank, AUDIO_AMBIENCE,
                           audio_volume(), 1.0f);
}

void audio_play(audio_event event, float volume, float pitch)
{
    if (!mixer_started || event < 0 || event >= AUDIO_EVENT_COUNT) return;
    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)event,
                           volume * audio_volume(), pitch);
}

void audio_stop(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    free_bank();
}

void audio_emergency_stop(void)
{
    /* The fatal-signal caller exits immediately; the sink observes EOF. */
}

void audio_toggle(const char *argv0)
{
    if (mixer_started) audio_stop();
    else audio_start(argv0);
}

bool audio_validate_assets(const char *argv0)
{
    bool valid;
    free_bank();
    valid = load_bank(argv0, true);
    free_bank();
    return valid;
}
