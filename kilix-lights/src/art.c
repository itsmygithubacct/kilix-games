/* art.c - strict P6 loading and sprite-shaped composition/picking. */
#include "art.h"

#include "draw.h"
#include "soft_raster.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum { SWITCH_W = 96, SWITCH_H = 96 };

static sr_canvas room;
static sr_canvas switch_rgb;
static sr_canvas switch_mask;
static bool loaded;

static bool copy_string(char *dst, size_t size, const char *src)
{
    int n;
    if (dst == NULL || size == 0 || src == NULL) return false;
    n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size;
}

static bool join_path(char *dst, size_t size, const char *a, const char *b)
{
    size_t nleft;
    int n;
    if (dst == NULL || a == NULL || b == NULL) return false;
    nleft = strlen(a);
    n = snprintf(dst, size, "%s%s%s", a,
                 nleft > 0 && a[nleft - 1] == '/' ? "" : "/", b);
    return n >= 0 && (size_t)n < size;
}

static bool dirname_of(const char *path, char *dst, size_t size)
{
    const char *slash;
    size_t n;
    if (path == NULL || path[0] == '\0') return false;
    slash = strrchr(path, '/');
    if (slash == NULL) return copy_string(dst, size, ".");
    n = (size_t)(slash - path);
    if (n == 0) return copy_string(dst, size, "/");
    if (n + 1u > size) return false;
    memcpy(dst, path, n);
    dst[n] = '\0';
    return true;
}

static bool executable_path(const char *argv0, char *dst, size_t size)
{
    char resolved[PATH_MAX];
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", resolved, sizeof resolved - 1u);
    if (n > 0 && (size_t)n < sizeof resolved) {
        resolved[n] = '\0';
        return copy_string(dst, size, resolved);
    }
#endif
    if (argv0 != NULL && strchr(argv0, '/') != NULL &&
        realpath(argv0, resolved) != NULL)
        return copy_string(dst, size, resolved);
    return false;
}

static bool load_bundle_from(const char *directory, bool verbose)
{
    char path[PATH_MAX];
    sr_canvas new_room = {0};
    sr_canvas new_switch = {0};
    sr_canvas new_mask = {0};
    size_t clear = 0;
    size_t solid = 0;
    size_t partial = 0;
    bool ok = true;

    if (!join_path(path, sizeof path, directory, "room.ppm") ||
        !sr_load_ppm(&new_room, path) || new_room.w != CANVAS_W ||
        new_room.h != CANVAS_H)
        ok = false;
    if (ok && (!join_path(path, sizeof path, directory, "switch.ppm") ||
               !sr_load_ppm(&new_switch, path) ||
               new_switch.w != SWITCH_W || new_switch.h != SWITCH_H))
        ok = false;
    if (ok && (!join_path(path, sizeof path, directory, "switch-mask.ppm") ||
               !sr_load_ppm(&new_mask, path) || new_mask.w != SWITCH_W ||
               new_mask.h != SWITCH_H))
        ok = false;
    if (ok) {
        for (int i = 0; i < SWITCH_W * SWITCH_H; i++) {
            uint32_t rgb = new_mask.px[i] & 0xffffffu;
            unsigned a = rgb & 0xffu;
            if (((rgb >> 16) & 0xffu) != a || ((rgb >> 8) & 0xffu) != a) {
                ok = false;
                break;
            }
            if (a == 0u) clear++;
            else if (a == 255u) solid++;
            else partial++;
            if (a == 0u && (new_switch.px[i] & 0xffffffu) != 0u) {
                ok = false;
                break;
            }
        }
        if (clear < 100u || solid < 2000u || partial < 10u) ok = false;
    }
    if (!ok) {
        sr_canvas_free(&new_room);
        sr_canvas_free(&new_switch);
        sr_canvas_free(&new_mask);
        return false;
    }
    room = new_room;
    switch_rgb = new_switch;
    switch_mask = new_mask;
    loaded = true;
    if (verbose)
        printf("art: loaded generated workshop and switch from %s\n",
               directory);
    return true;
}

static bool try_asset_locations(const char *argv0, bool verbose)
{
    char executable[PATH_MAX];
    char directory[PATH_MAX];
    char candidate[PATH_MAX];
    const char *override = getenv("KILIX_LIGHTS_ASSETS");

    if (override != NULL && override[0] != '\0') {
        if (load_bundle_from(override, verbose)) return true;
        if (join_path(candidate, sizeof candidate, override, "art") &&
            load_bundle_from(candidate, verbose))
            return true;
    }
    if (executable_path(argv0, executable, sizeof executable) &&
        dirname_of(executable, directory, sizeof directory) &&
        join_path(candidate, sizeof candidate, directory, "../assets/art") &&
        load_bundle_from(candidate, verbose))
        return true;
    return load_bundle_from("assets/art", verbose);
}

bool art_init(const char *argv0, bool verbose)
{
    art_shutdown();
    if (try_asset_locations(argv0, verbose)) return true;
    if (verbose)
        fprintf(stderr, "art: generated asset bundle missing or malformed; "
                        "using built-in fallback\n");
    return false;
}

void art_shutdown(void)
{
    sr_canvas_free(&room);
    sr_canvas_free(&switch_rgb);
    sr_canvas_free(&switch_mask);
    loaded = false;
}

bool art_ready(void)
{
    return loaded;
}

bool art_validate(const char *argv0, bool verbose)
{
    bool was_loaded = loaded;
    if (!was_loaded && !art_init(argv0, verbose)) return false;
    if (!was_loaded) art_shutdown();
    return true;
}

static uint8_t mix_channel(uint8_t a, uint8_t b, unsigned alpha)
{
    return (uint8_t)(((unsigned)a * (255u - alpha) +
                      (unsigned)b * alpha + 127u) / 255u);
}

static void blend_pixel(Canvas *canvas, int x, int y, uint32_t rgb,
                        unsigned alpha)
{
    uint32_t old;
    uint8_t r, g, b;
    if (x < 0 || y < 0 || x >= canvas->w || y >= canvas->h) return;
    if (alpha > 255u) alpha = 255u;
    old = canvas->px[(size_t)y * (size_t)canvas->w + (size_t)x];
    r = mix_channel((uint8_t)(old >> 16), (uint8_t)(rgb >> 16), alpha);
    g = mix_channel((uint8_t)(old >> 8), (uint8_t)(rgb >> 8), alpha);
    b = mix_channel((uint8_t)old, (uint8_t)rgb, alpha);
    canvas->px[(size_t)y * (size_t)canvas->w + (size_t)x] =
        0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void art_draw_room(Canvas *canvas)
{
    if (canvas == NULL || canvas->px == NULL) return;
    if (loaded) {
        memcpy(canvas->px, room.px,
               (size_t)CANVAS_W * CANVAS_H * sizeof canvas->px[0]);
        return;
    }
    draw_gradient_v(canvas, 0, 0, CANVAS_W, CANVAS_H, 0x14252a, 0x20170f);
    draw_round_rect(canvas, 38, 44, 410, 310, 12, 0x173238);
    draw_frame(canvas, 38, 44, 410, 310, 3, 0xb5843a);
    draw_round_rect(canvas, 456, 62, 104, 278, 10, 0x173238);
    draw_frame(canvas, 456, 62, 104, 278, 3, 0xb5843a);
}

static unsigned mask_at_scaled(int dx, int dy, int size)
{
    int sx;
    int sy;
    if (!loaded || size <= 0 || dx < 0 || dy < 0 || dx >= size || dy >= size)
        return 0u;
    sx = iclampi(dx * SWITCH_W / size, 0, SWITCH_W - 1);
    sy = iclampi(dy * SWITCH_H / size, 0, SWITCH_H - 1);
    return switch_mask.px[sy * SWITCH_W + sx] & 0xffu;
}

bool art_switch_hit(int x, int y, int size, int px, int py)
{
    int dx = px - x;
    int dy = py - y;
    if (dx < 0 || dy < 0 || dx >= size || dy >= size) return false;
    if (!loaded) {
        int corner = imaxi(2, size / 9);
        return !((dx < corner && dy < corner) ||
                 (dx >= size - corner && dy < corner) ||
                 (dx < corner && dy >= size - corner) ||
                 (dx >= size - corner && dy >= size - corner));
    }
    return mask_at_scaled(dx, dy, size) >= 96u;
}

static uint32_t tinted(uint32_t rgb, bool on, bool pressed)
{
    unsigned r = (rgb >> 16) & 0xffu;
    unsigned g = (rgb >> 8) & 0xffu;
    unsigned b = rgb & 0xffu;
    if (on) {
        r = imini(255, (int)(r * 110u / 100u + 12u));
        g = imini(255, (int)(g * 103u / 100u + 5u));
        b = b * 78u / 100u;
    } else {
        r = r * 48u / 100u;
        g = imini(255, (int)(g * 58u / 100u + 6u));
        b = imini(255, (int)(b * 66u / 100u + 10u));
    }
    if (pressed) {
        r = r * 72u / 100u;
        g = g * 72u / 100u;
        b = b * 72u / 100u;
    }
    return (r << 16) | (g << 8) | b;
}

void art_draw_switch(Canvas *canvas, int x, int y, int size,
                     bool on, bool hover, bool focused, bool pressed)
{
    uint32_t halo = focused ? 0x9de8ffu : 0xf4c66au;
    if (canvas == NULL || canvas->px == NULL || size <= 0) return;

    if (!loaded) {
        if (on) draw_blend_disc(canvas, x + size / 2, y + size / 2,
                                size * 3 / 5, 0xffbd58, 45u);
        draw_round_rect(canvas, x, y, size, size, imaxi(3, size / 8),
                        on ? 0xd9aa52 : 0x274b50);
        draw_frame(canvas, x + 2, y + 2, size - 4, size - 4, 2,
                   hover || focused ? halo : 0x8a6730);
        return;
    }

    if (hover || focused) {
        int radius = focused ? 3 : 2;
        for (int dy = -radius; dy < size + radius; dy++) {
            for (int dx = -radius; dx < size + radius; dx++) {
                if (mask_at_scaled(dx, dy, size) >= 96u) continue;
                bool neighbor = false;
                for (int oy = -radius; oy <= radius && !neighbor; oy++)
                    for (int ox = -radius; ox <= radius; ox++)
                        if (mask_at_scaled(dx + ox, dy + oy, size) >= 160u) {
                            neighbor = true;
                            break;
                        }
                if (neighbor)
                    blend_pixel(canvas, x + dx, y + dy, halo,
                                focused ? 225u : 165u);
            }
        }
    }

    for (int dy = 0; dy < size; dy++) {
        int sy = iclampi(dy * SWITCH_H / size, 0, SWITCH_H - 1);
        for (int dx = 0; dx < size; dx++) {
            int sx = iclampi(dx * SWITCH_W / size, 0, SWITCH_W - 1);
            size_t source = (size_t)sy * SWITCH_W + (size_t)sx;
            unsigned alpha = switch_mask.px[source] & 0xffu;
            if (alpha == 0u) continue;
            blend_pixel(canvas, x + dx, y + dy,
                        tinted(switch_rgb.px[source], on, pressed), alpha);
        }
    }
}
