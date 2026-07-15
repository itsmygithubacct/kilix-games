/* Banked PCM playback for the python-authored bank in assets/sfx/.
 *
 * A mixer thread sums active voices into a small buffer and writes s16le to a
 * forked sink (pacat/pw-play/aplay/play). There is deliberately NO procedural
 * fallback: the user asked for python-generated sounds, so if the bank is
 * missing we go silent and say why rather than quietly substituting C-synth
 * beeps that would make a broken install sound fine.
 */
#include "kilix_pong.h"
#include "pcm_wav.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIX_FRAMES 384
#define MAX_VOICES 16

typedef struct {
    int16_t *data;
    int length;
} Sample;

typedef struct {
    const int16_t *data;
    int length;
    float position, step, volume;
    bool active;
} Voice;

static Sample samples[SFX_COUNT][SFX_VARIANTS];
static uint8_t sample_counts[SFX_COUNT];
static uint8_t next_variant[SFX_COUNT];
static Voice voices[MAX_VOICES];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer_thread;
static atomic_bool running = ATOMIC_VAR_INIT(false);
static bool thread_started;
static bool enabled = true;
/* Two distinct facts, deliberately not conflated: bank_complete means all
   SFX_COUNT*SFX_VARIANTS production WAVs loaded, which is what --asset-check
   must attest; bank_playable means at least one did, which is all playback
   needs. Reporting a 1-of-21 bank as "loaded" would make the asset validator
   weaker than its name. */
static bool bank_complete;
static bool bank_playable;
static int sink_fd = -1;
static pid_t sink_pid = -1;
static const char *sink_label;
static uint32_t rotate_rng = 0x9E3779B9u;

/* Filenames match tools/gen_sfx.py: variant 1 is the bare cue name, later
   variants take the _vNN suffix. Order matches the SFX_* enum. */
static const char *const CUE_NAMES[SFX_COUNT] = {
    "paddle", "wall", "score", "serve", "miss", "menu", "gameover"
};

const char *sound_sink_name(void) { return sink_label; }
/* Attests a COMPLETE production bank; --asset-check relies on this. */
bool sound_bank_loaded(void) { return bank_complete; }
bool sound_is_enabled(void) { return enabled && bank_playable && sink_fd >= 0; }
void sound_set_enabled(bool on) { enabled = on; }

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
            int frames = 0;
            int16_t *data = pcm_wav_load(path, &frames, err, sizeof err);
            if (!data) {
                if (missing < 3)   /* one line per problem, but do not spam 21 */
                    fprintf(stderr, "kilix-pong: sfx: %s\n", err);
                missing++;
                continue;
            }
            samples[cue][v].data = data;
            samples[cue][v].length = frames;
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

/* Probe order matches the house convention in kitty-breakout. pw-play is kept
   in the list even though it is absent on the authoring box: its presence here
   is untested, and that is recorded rather than papered over. */
static bool open_sink(void)
{
    static const struct {
        const char *name;
        const char *argv[12];   /* longest is aplay's 10 args + NULL */
    } sinks[] = {
        { "pacat",   { "pacat", "--raw", "--latency-msec=18", "--rate=44100",
                       "--channels=1", "--format=s16le", NULL } },
        { "pw-play", { "pw-play", "--raw", "--rate=44100", "--channels=1",
                       "--format=s16", "-", NULL } },
        { "aplay",   { "aplay", "-q", "-f", "S16_LE", "-r", "44100", "-c", "1",
                       "-B", "30000", NULL } },
        { "play",    { "play", "-q", "-t", "s16", "-r", "44100", "-c", "1",
                       "-", NULL } },
    };

    for (size_t i = 0; i < sizeof sinks / sizeof sinks[0]; i++) {
        int fds[2];
        if (pipe(fds) != 0) continue;

        pid_t pid = fork();
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            continue;
        }
        if (pid == 0) {
            /* Child: stdin from the pipe, output silenced so a sink's chatter
               never corrupts the kitty graphics stream on stdout. */
            dup2(fds[0], STDIN_FILENO);
            close(fds[0]);
            close(fds[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            signal(SIGPIPE, SIG_DFL);
            execvp(sinks[i].argv[0], (char *const *)sinks[i].argv);
            _exit(127);
        }

        close(fds[0]);

        /* If the binary is absent the child exits 127 almost immediately.
           Give it a moment, then check before trusting the pipe. */
        usleep(60000);
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            close(fds[1]);
            continue;   /* that sink is not really available */
        }

        sink_fd = fds[1];
        sink_pid = pid;
        sink_label = sinks[i].name;
        return true;
    }
    return false;
}

static void *mixer_main(void *arg)
{
    (void)arg;
    static int32_t accum[MIX_FRAMES];
    static int16_t out[MIX_FRAMES];

    while (atomic_load(&running)) {
        memset(accum, 0, sizeof accum);

        pthread_mutex_lock(&lock);
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice *voice = &voices[v];
            if (!voice->active) continue;
            for (int i = 0; i < MIX_FRAMES; i++) {
                int index = (int)voice->position;
                if (index >= voice->length) {
                    voice->active = false;
                    break;
                }
                accum[i] += (int32_t)(voice->data[index] * voice->volume);
                voice->position += voice->step;
            }
        }
        pthread_mutex_unlock(&lock);

        for (int i = 0; i < MIX_FRAMES; i++) {
            int32_t s = accum[i];
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[i] = (int16_t)s;
        }

        const uint8_t *p = (const uint8_t *)out;
        size_t remaining = sizeof out;
        while (remaining > 0 && atomic_load(&running)) {
            ssize_t n = write(sink_fd, p, remaining);
            if (n > 0) {
                p += n;
                remaining -= (size_t)n;
            } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            } else {
                atomic_store(&running, false);   /* sink died; go quiet */
                break;
            }
        }
    }
    return NULL;
}

bool sound_init(void)
{
    if (!load_bank()) return false;

    if (!open_sink()) {
        fprintf(stderr, "kilix-pong: no audio sink (tried pacat, pw-play, "
                        "aplay, play); running silent\n");
        return false;
    }

    /* A dying sink must not take the game with it. */
    signal(SIGPIPE, SIG_IGN);

    atomic_store(&running, true);
    if (pthread_create(&mixer_thread, NULL, mixer_main, NULL) != 0) {
        atomic_store(&running, false);
        fprintf(stderr, "kilix-pong: cannot start mixer thread; running silent\n");
        return false;
    }
    thread_started = true;
    return true;
}

void sound_shutdown(void)
{
    atomic_store(&running, false);
    if (thread_started) {
        pthread_join(mixer_thread, NULL);
        thread_started = false;
    }
    if (sink_fd >= 0) {
        close(sink_fd);
        sink_fd = -1;
    }
    if (sink_pid > 0) {
        kill(sink_pid, SIGTERM);
        waitpid(sink_pid, NULL, 0);
        sink_pid = -1;
    }
    sink_label = NULL;
    free_bank();
    bank_complete = false;
    bank_playable = false;
}

void sound_play(int id, float volume, float pitch)
{
    if (id < 0 || id >= SFX_COUNT) return;
    if (!enabled || !bank_playable || sink_fd < 0 || !atomic_load(&running)) return;

    int count = sample_counts[id];
    if (count <= 0) return;

    /* Rotate variants, but never repeat the one just used: a strict cycle is
       audible as a pattern during a long rally, pure random repeats too often. */
    int choice = next_variant[id];
    if (count > 2)
        choice = (choice + 1 + (int)(rotate_random() % (uint32_t)(count - 1))) % count;
    else if (count == 2)
        choice = choice ^ 1;
    next_variant[id] = (uint8_t)choice;

    Sample *sample = &samples[id][choice];
    if (!sample->data) {
        for (int v = 0; v < SFX_VARIANTS; v++)   /* variant gap: take any */
            if (samples[id][v].data) { sample = &samples[id][v]; break; }
        if (!sample->data) return;
    }

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 4.0f) volume = 4.0f;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f) pitch = 4.0f;

    pthread_mutex_lock(&lock);
    Voice *slot = NULL;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].active) { slot = &voices[v]; break; }
    }
    if (!slot) {
        /* All voices busy: steal the most nearly finished one. */
        float best = -1.0f;
        for (int v = 0; v < MAX_VOICES; v++) {
            float done = voices[v].length ? voices[v].position / (float)voices[v].length : 1.0f;
            if (done > best) { best = done; slot = &voices[v]; }
        }
    }
    slot->data = sample->data;
    slot->length = sample->length;
    slot->position = 0.0f;
    slot->step = pitch;
    slot->volume = volume;
    slot->active = true;
    pthread_mutex_unlock(&lock);
}
