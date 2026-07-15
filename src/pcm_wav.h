/* Minimal RIFF/WAVE reader for the python-authored bank in assets/sfx/.
 *
 * Deliberately strict: this only accepts the exact format tools/gen_sfx.py
 * emits (PCM, mono, 16-bit, 44100 Hz). A bank that does not match is a bug in
 * the generator or a corrupted asset, and silently resampling or downmixing it
 * would hide that. Callers treat a NULL return as "degrade to silence and say
 * so", never as "synthesise something instead".
 */
#ifndef PCM_WAV_H
#define PCM_WAV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCM_WAV_RATE     44100
#define PCM_WAV_CHANNELS 1
#define PCM_WAV_BITS     16

/* Guards a corrupt/hostile header from asking for a huge allocation.
   ~30s of mono 44.1kHz audio; the longest real cue is 0.45s. */
#define PCM_WAV_MAX_FRAMES (PCM_WAV_RATE * 30)

static inline uint16_t pcm_wav_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t pcm_wav_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reads path into a freshly allocated int16 buffer. Returns NULL on any
 * malformed or unexpected input, writing a reason into err when provided.
 * *out_frames receives the sample count on success. */
static inline int16_t *pcm_wav_load(const char *path, int *out_frames,
                                    char *err, size_t err_len)
{
    *out_frames = 0;

#define PCM_FAIL(...)                                          \
    do {                                                       \
        if (err && err_len) snprintf(err, err_len, __VA_ARGS__); \
        free(bytes);                                           \
        if (fp) fclose(fp);                                    \
        return NULL;                                           \
    } while (0)

    uint8_t *bytes = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) PCM_FAIL("cannot open %s", path);

    if (fseek(fp, 0, SEEK_END) != 0) PCM_FAIL("cannot seek %s", path);
    long size = ftell(fp);
    if (size < 44 || size > (1L << 26)) PCM_FAIL("implausible size for %s", path);
    rewind(fp);

    bytes = (uint8_t *)malloc((size_t)size);
    if (!bytes) PCM_FAIL("out of memory reading %s", path);
    if (fread(bytes, 1, (size_t)size, fp) != (size_t)size)
        PCM_FAIL("short read on %s", path);
    fclose(fp);
    fp = NULL;

    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0)
        PCM_FAIL("not a RIFF/WAVE file: %s", path);

    /* Walk chunks rather than assuming a canonical 44-byte header: the wave
       module is free to emit LIST/fact chunks before data. */
    bool have_fmt = false;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    size_t pos = 12;

    while (pos + 8 <= (size_t)size) {
        const uint8_t *id = bytes + pos;
        uint32_t len = pcm_wav_u32(bytes + pos + 4);
        size_t body = pos + 8;
        if (len > (uint32_t)size || body + len > (size_t)size)
            PCM_FAIL("chunk overruns file: %s", path);

        if (memcmp(id, "fmt ", 4) == 0) {
            if (len < 16) PCM_FAIL("short fmt chunk: %s", path);
            uint16_t format = pcm_wav_u16(bytes + body);
            uint16_t channels = pcm_wav_u16(bytes + body + 2);
            uint32_t rate = pcm_wav_u32(bytes + body + 4);
            uint16_t bits = pcm_wav_u16(bytes + body + 14);
            if (format != 1)
                PCM_FAIL("%s is not PCM (format %u)", path, format);
            if (channels != PCM_WAV_CHANNELS || rate != PCM_WAV_RATE ||
                bits != PCM_WAV_BITS)
                PCM_FAIL("%s is %uch/%ubit/%uHz, expected %dch/%dbit/%dHz",
                         path, channels, bits, rate,
                         PCM_WAV_CHANNELS, PCM_WAV_BITS, PCM_WAV_RATE);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data = bytes + body;
            data_len = len;
        }

        pos = body + len + (len & 1u);   /* chunks are word-aligned */
    }

    if (!have_fmt) PCM_FAIL("no fmt chunk: %s", path);
    if (!data) PCM_FAIL("no data chunk: %s", path);
    if (data_len < 2) PCM_FAIL("empty data chunk: %s", path);

    int frames = (int)(data_len / 2u);
    if (frames > PCM_WAV_MAX_FRAMES) PCM_FAIL("%s is too long", path);

    int16_t *samples = (int16_t *)malloc((size_t)frames * sizeof(int16_t));
    if (!samples) PCM_FAIL("out of memory decoding %s", path);
    for (int i = 0; i < frames; i++)
        samples[i] = (int16_t)pcm_wav_u16(data + (size_t)i * 2);

    free(bytes);
    *out_frames = frames;
    return samples;

#undef PCM_FAIL
}

#endif
