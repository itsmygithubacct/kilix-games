/* draw.c - see draw.h.
 *
 * The Neuronte half is adapted from kilix-zell's src/draw_neuronte.c (MIT,
 * Copyright (c) 2026 itsmygithubacct), the renderer of zell-neuronte: the
 * same text cells, block-glyph bars and outlines, fitted disc, and octant
 * picture cells, called directly instead of through soft-raster's names.
 */
#include "draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PUZZLLM_NEURONTE
#include "neuronte.h"
#ifndef PUZZLLM_NEURONTE_WEIGHTS_DEFAULT
#define PUZZLLM_NEURONTE_WEIGHTS_DEFAULT "neuronte-split-v4.nrt"
#endif
#endif

enum { CW = DRAW_CELL_W, CH = DRAW_CELL_H };

static bool force_raster;

/* ---- plain code: fills and compositing, shared by both renderers ---- */

static int iround(float v)
{
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static int alpha255(float a)
{
    return a <= 0.0f ? 0 : a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f);
}

static inline void blend_px(uint32_t *to, uint32_t rgb, int a)
{
    uint32_t d = *to;
    int inv = 255 - a;
    int r = ((int)((rgb >> 16) & 255u) * a + (int)((d >> 16) & 255u) * inv) / 255;
    int g = ((int)((rgb >> 8) & 255u) * a + (int)((d >> 8) & 255u) * inv) / 255;
    int b = ((int)(rgb & 255u) * a + (int)(d & 255u) * inv) / 255;
    *to = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* An integer rectangle, clipped; one colour, blended by alpha. */
static void fill_box(sr_canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb, int a)
{
    if (c == NULL || c->px == NULL || a <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->w;
        if (a >= 255) {
            uint32_t px = 0xff000000u | (rgb & 0xffffffu);
            for (int x = x0; x < x1; x++) row[x] = px;
        } else {
            for (int x = x0; x < x1; x++) blend_px(&row[x], rgb, a);
        }
    }
}

static void plain_fill(sr_canvas *c, float x, float y, float w, float h, uint32_t rgb, float alpha)
{
    fill_box(c, iround(x), iround(y), iround(x + w), iround(y + h), rgb, alpha255(alpha));
}

/* soft-raster's own mix for a premultiplied source, as in Zell. */
static inline void composite(uint32_t *to, uint32_t s, int ga)
{
    int sa = ((int)(s >> 24) * ga) / 255;
    int inv, r, g, b, a;
    uint32_t d;

    if (sa <= 0) return;
    if (sa >= 255) { *to = s; return; }
    inv = 255 - sa;
    d = *to;
    r = ((int)((s >> 16) & 255u) * ga) / 255 + ((int)((d >> 16) & 255u) * inv) / 255;
    g = ((int)((s >> 8) & 255u) * ga) / 255 + ((int)((d >> 8) & 255u) * inv) / 255;
    b = ((int)(s & 255u) * ga) / 255 + ((int)(d & 255u) * inv) / 255;
    a = sa + ((int)(d >> 24) * inv) / 255;
    *to = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static size_t utf8_next(const char *s, size_t len, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    int n;
    uint32_t v;

    if (len == 0) { *cp = 0; return 0; }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xe0) == 0xc0) { n = 2; v = p[0] & 0x1fu; }
    else if ((p[0] & 0xf0) == 0xe0) { n = 3; v = p[0] & 0x0fu; }
    else if ((p[0] & 0xf8) == 0xf0) { n = 4; v = p[0] & 0x07u; }
    else { *cp = 0xfffd; return 1; }
    if ((size_t)n > len) { *cp = 0xfffd; return len; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) { *cp = 0xfffd; return (size_t)i; }
        v = (v << 6) | (p[i] & 0x3fu);
    }
    *cp = v;
    return (size_t)n;
}

/* ---- the raster renderer ---- */

static void raster_bits(sr_canvas *c, int x, int y, unsigned char ch, uint32_t fg, int a, bool bold)
{
    const uint8_t *rows = sr_font_glyph_in(SR_FONT_FIXED_8X16, ch);
    if (rows == NULL) return;
    for (int j = 0; j < CH; j++) {
        uint8_t bits = rows[j];
        if (bold) bits |= (uint8_t)(bits >> 1);
        if (y + j < 0 || y + j >= c->h) continue;
        for (int i = 0; i < CW; i++) {
            if ((bits & (0x80 >> i)) && x + i >= 0 && x + i < c->w) {
                uint32_t *px = &c->px[(size_t)(y + j) * (size_t)c->w + (size_t)(x + i)];
                if (a >= 255) *px = 0xff000000u | fg;
                else blend_px(px, fg, a);
            }
        }
    }
}

/* Box drawing U+2500..U+257F as arms: up, right, down, left; 0 none,
 * 1 light, 2 heavy, 3 double. */
static bool box_arms(uint32_t cp, int arms[4])
{
    static const struct { uint16_t cp; uint8_t u, r, d, l; } table[] = {
        {0x2500, 0,1,0,1}, {0x2501, 0,2,0,2}, {0x2502, 1,0,1,0}, {0x2503, 2,0,2,0},
        {0x2504, 0,1,0,1}, {0x2505, 0,2,0,2}, {0x2506, 1,0,1,0}, {0x2507, 2,0,2,0},
        {0x2508, 0,1,0,1}, {0x2509, 0,2,0,2}, {0x250a, 1,0,1,0}, {0x250b, 2,0,2,0},
        {0x250c, 0,1,1,0}, {0x250f, 0,2,2,0}, {0x2510, 0,0,1,1}, {0x2513, 0,0,2,2},
        {0x2514, 1,1,0,0}, {0x2517, 2,2,0,0}, {0x2518, 1,0,0,1}, {0x251b, 2,0,0,2},
        {0x251c, 1,1,1,0}, {0x2523, 2,2,2,0}, {0x2524, 1,0,1,1}, {0x252b, 2,0,2,2},
        {0x252c, 0,1,1,1}, {0x2533, 0,2,2,2}, {0x2534, 1,1,0,1}, {0x253b, 2,2,0,2},
        {0x253c, 1,1,1,1}, {0x254b, 2,2,2,2}, {0x254c, 0,1,0,1}, {0x254e, 1,0,1,0},
        {0x2550, 0,3,0,3}, {0x2551, 3,0,3,0}, {0x2554, 0,3,3,0}, {0x2557, 0,0,3,3},
        {0x255a, 3,3,0,0}, {0x255d, 3,0,0,3}, {0x2560, 3,3,3,0}, {0x2563, 3,0,3,3},
        {0x2566, 0,3,3,3}, {0x2569, 3,3,0,3}, {0x256c, 3,3,3,3},
        {0x256d, 0,1,1,0}, {0x256e, 0,0,1,1}, {0x256f, 1,0,0,1}, {0x2570, 1,1,0,0},
        {0x2574, 0,0,0,1}, {0x2575, 1,0,0,0}, {0x2576, 0,1,0,0}, {0x2577, 0,0,1,0},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].cp == cp) {
            arms[0] = table[i].u; arms[1] = table[i].r;
            arms[2] = table[i].d; arms[3] = table[i].l;
            return true;
        }
    }
    return false;
}

static void raster_box(sr_canvas *c, int x, int y, const int arms[4], uint32_t fg)
{
    int cx = x + CW / 2 - 1, cy = y + CH / 2 - 1;
    for (int a = 0; a < 4; a++) {
        int w = arms[a] == 2 ? 3 : 1;
        if (arms[a] == 0) continue;
        for (int pass = 0; pass < (arms[a] == 3 ? 2 : 1); pass++) {
            int o = arms[a] == 3 ? (pass ? 2 : -2) : (w == 3 ? -1 : 0);
            if (a == 0) fill_box(c, cx + o, y, cx + o + w, cy + 1, fg, 255);
            if (a == 2) fill_box(c, cx + o, cy, cx + o + w, y + CH, fg, 255);
            if (a == 1) fill_box(c, cx, cy + o, x + CW, cy + o + w, fg, 255);
            if (a == 3) fill_box(c, x, cy + o, cx + 1, cy + o + w, fg, 255);
        }
    }
}

/* Characters the 8x16 font lacks, mapped to a close ASCII. */
static unsigned char ascii_stand_in(uint32_t cp)
{
    switch (cp) {
    case 0x2022: case 0x25cf: case 0x23fa: case 0x2219: case 0x00b7: return '*';
    case 0x25cb: case 0x25ef: case 0x25e6: return 'o';
    case 0x2026: return '.';
    case 0x2713: case 0x2714: case 0x221a: return 'v';
    case 0x2717: case 0x2718: case 0x00d7: return 'x';
    case 0x276f: case 0x203a: case 0x2192: case 0x25b6: case 0x25b8: case 0x21b3: return '>';
    case 0x2039: case 0x2190: case 0x25c0: return '<';
    case 0x2191: case 0x25b2: return '^';
    case 0x2193: case 0x25bc: return 'v';
    case 0x2018: case 0x2019: case 0x00b4: return '\'';
    case 0x201c: case 0x201d: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x00a0: return ' ';
    default: return '?';
    }
}

static void raster_glyph(sr_canvas *c, int x, int y, uint32_t cp, uint32_t fg, int a, bool bold)
{
    int arms[4];
    if (cp <= 0x20) return;
    if (cp <= 0x7e) {
        raster_bits(c, x, y, (unsigned char)cp, fg, a, bold);
    } else if (cp >= 0x2500 && cp <= 0x257f) {
        if (box_arms(cp, arms)) raster_box(c, x, y, arms, fg);
        else raster_bits(c, x, y, '+', fg, a, false);
    } else if (cp >= 0x2580 && cp <= 0x259f) {
        if (cp == 0x2588) fill_box(c, x, y, x + CW, y + CH, fg, a);
        else if (cp == 0x2580) fill_box(c, x, y, x + CW, y + CH / 2, fg, a);
        else if (cp >= 0x2581 && cp <= 0x2587) {
            int h = (int)(cp - 0x2580) * CH / 8;
            fill_box(c, x, y + CH - h, x + CW, y + CH, fg, a);
        } else if (cp >= 0x2589 && cp <= 0x258f) {
            int w = (int)(0x2590 - cp) * CW / 8;
            fill_box(c, x, y, x + w, y + CH, fg, a);
        } else if (cp == 0x2590) fill_box(c, x + CW / 2, y, x + CW, y + CH, fg, a);
        else if (cp >= 0x2591 && cp <= 0x2593) {
            fill_box(c, x, y, x + CW, y + CH, fg, a * (int)(cp - 0x2590) / 4);
        } else raster_bits(c, x, y, '#', fg, a, false);
    } else if (cp >= 0x2800 && cp <= 0x28ff) {
        /* braille: eight dots in a 2x4 grid (spinners) */
        static const int dx[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static const int dy[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        for (int b = 0; b < 8; b++)
            if ((cp - 0x2800) & (1u << b)) {
                int px = x + 1 + dx[b] * 4, py = y + 2 + dy[b] * 3;
                fill_box(c, px, py, px + 2, py + 2, fg, a);
            }
    } else {
        raster_bits(c, x, y, ascii_stand_in(cp), fg, a, false);
    }
}

static void raster_picture(sr_canvas *c, const uint32_t *px, int sw, int sh,
                           int x, int y, int w, int h, int ga)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        const uint32_t *src;
        if (yy < 0 || yy >= c->h) continue;
        src = px + (size_t)((long long)j * sh / h) * (size_t)sw;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= c->w) continue;
            composite(&c->px[(size_t)yy * (size_t)c->w + (size_t)xx],
                      src[(long long)i * sw / w], ga);
        }
    }
}

static void raster_disc(sr_canvas *c, float cx, float cy, float rx, float ry, uint32_t rgb, int a)
{
    if (rx <= 0 || ry <= 0) return;
    for (int y = (int)floorf(cy - ry); y <= (int)ceilf(cy + ry); y++) {
        float t = ((float)y + 0.5f - cy) / ry;
        float half;
        if (t <= -1.0f || t >= 1.0f) continue;
        half = rx * sqrtf(1.0f - t * t);
        fill_box(c, iround(cx - half), y, iround(cx + half), y + 1, rgb, a);
    }
}

/* ---- the Neuronte renderer ---- */

#ifdef PUZZLLM_NEURONTE
static neuronte *font;
static int font_state;   /* 0 not tried, 1 open, -1 failed */
static char font_path[1024];

static const char *weights_path(void)
{
    const char *p = getenv("PUZZLLM_NEURONTE_WEIGHTS");
    if (p == NULL || p[0] == '\0') p = getenv("ZELL_NEURONTE_WEIGHTS");
    if (p == NULL || p[0] == '\0') p = PUZZLLM_NEURONTE_WEIGHTS_DEFAULT;
    return p;
}

static bool nn(void)
{
    return font_state > 0 && !force_raster;
}

static uint32_t nn_color(uint32_t rgb, float alpha)
{
    return ((uint32_t)alpha255(alpha) << 24) | (rgb & 0xffffffu);
}

static void nn_scalar(sr_canvas *c, uint32_t scalar, int x, int y, int w, int h,
                      uint32_t rgb, float alpha, unsigned style)
{
    if (c == NULL || w < 1 || h < 1) return;
    neuronte_draw_scalar_styled(font, c->px, c->w, c->w, c->h, x, y, w, h, scalar,
                                nn_color(rgb, alpha), style);
}

/* Where the black circle's ink sits in its cell, measured once from the
 * net's own coverage, so a disc can be fitted to an ellipse's box. */
static bool disc_box(float box[4])
{
    enum { W = 48, H = 96 };
    static float saved[4];
    static int state;
    if (state == 0) {
        static float cov[W * H];
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        state = -1;
        if (neuronte_coverage(font, 0x25cfu, W, H, 0, cov) == 0) {
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (cov[y * W + x] > 0.5f) {
                        x0 = x < x0 ? x : x0; y0 = y < y0 ? y : y0;
                        x1 = x > x1 ? x : x1; y1 = y > y1 ? y : y1;
                    }
            if (x1 > x0 && y1 > y0) {
                saved[0] = (float)x0 / W; saved[1] = (float)y0 / H;
                saved[2] = (float)(x1 + 1) / W; saved[3] = (float)(y1 + 1) / H;
                state = 1;
            }
        }
    }
    if (state < 0) return false;
    memcpy(box, saved, sizeof saved);
    return true;
}

/* Which of a 2 x 4 cell's eight colours each of its pixels takes, from
 * three masks of the pixel net used as bit planes (see Zell). */
static const unsigned char *cell_labels(void)
{
    static const uint32_t planes[3] = {0x2590u, 0x1cd33u, 0x2584u};
    static unsigned char labels[8];
    static int state;
    if (state == 0) {
        float cov[8];
        memset(labels, 0, sizeof labels);
        state = 1;
        for (int plane = 0; plane < 3 && state > 0; plane++) {
            if (neuronte_coverage(font, planes[plane], 2, 4, 0, cov) != 0) {
                state = -1;
                break;
            }
            for (int i = 0; i < 8; i++)
                labels[i] = (unsigned char)(labels[i] | ((cov[i] > 0.5f) << plane));
        }
    }
    return state > 0 ? labels : NULL;
}

/* A scaled picture, cell by cell: each 2 x 4 cell's sub-cells take the
 * source pixels nearest-neighbour scaling puts there, and the net's masks
 * say which sub-cell each pixel shows. False when the masks are missing. */
static bool nn_picture(sr_canvas *dst, const uint32_t *px, int sw, int sh,
                       int x, int y, int w, int h, int ga)
{
    static const unsigned char number[8] = {0, 1, 2, 3, 6, 7, 4, 5};
    enum { MAX_SIDE = 4096 };
    static int col_of[MAX_SIDE];
    const unsigned char *labels = cell_labels();
    unsigned char from[8];
    int dx0, dy0, dx1, dy1;

    if (labels == NULL || w > MAX_SIDE || h > MAX_SIDE) return false;
    for (int k = 0; k < 8; k++) {
        from[k] = 8;
        for (int j = 0; j < 8; j++)
            if (number[j] == labels[k]) from[k] = (unsigned char)j;
        if (from[k] > 7) return false;
    }
    dx0 = x < 0 ? -x : 0;
    dy0 = y < 0 ? -y : 0;
    dx1 = x + w > dst->w ? dst->w - x : w;
    dy1 = y + h > dst->h ? dst->h - y : h;
    for (int dx = dx0 & ~1; dx < w && dx <= (dx1 | 1); dx++)
        col_of[dx] = (int)(((long long)dx * sw) / w);
    for (int dy = dy0; dy < dy1; dy++) {
        int row_in_cell = dy & 3;
        int cell_y = dy - row_in_cell;
        uint32_t *out = dst->px + (size_t)(y + dy) * (size_t)dst->w + (size_t)x;
        const uint32_t *row_of[4];
        for (int k = 0; k < 4; k++) {
            int py = cell_y + k;
            row_of[k] = py < h ? px + (size_t)(((long long)py * sh) / h) * (size_t)sw : NULL;
        }
        for (int dx = dx0; dx < dx1; dx++) {
            int sub = from[row_in_cell * 2 + (dx & 1)];
            int sx = (dx & ~1) + (sub & 1);
            const uint32_t *row = row_of[sub >> 1];
            if (sx < w && row != NULL) composite(&out[dx], row[col_of[sx]], ga);
        }
    }
    return true;
}
#endif

#ifdef PUZZLLM_NEURONTE
/* The nets draw a character once per cell size and the library keeps the
 * mask; drawing the common ones now keeps that first-use cost out of the
 * first frames. */
static void warm_cache(void)
{
    static const uint32_t extra[] = {
        0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, 0x251c, 0x2524, 0x252c,
        0x2534, 0x253c, 0x256d, 0x256e, 0x256f, 0x2570, 0x2580, 0x2584, 0x2588,
        0x258c, 0x2590, 0x2591, 0x2592, 0x2593, 0x25cf, 0x2022, 0x2026, 0x2192,
        0x276f, 0x2713, 0x2717, 0x2800, 0x280b, 0x2819, 0x2839, 0x2838, 0x283c,
        0x2834, 0x2826, 0x2827, 0x2807, 0x280f};
    uint32_t px[CW * CH];
    for (uint32_t cp = 0x21; cp < 0x7f; cp++) {
        neuronte_draw_scalar_styled(font, px, CW, CW, CH, 0, 0, CW, CH, cp, 0xffffffffu, 0);
        neuronte_draw_scalar_styled(font, px, CW, CW, CH, 0, 0, CW, CH, cp, 0xffffffffu,
                                    NEURONTE_STYLE_BOLD);
    }
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++)
        neuronte_draw_scalar_styled(font, px, CW, CW, CH, 0, 0, CW, CH, extra[i], 0xffffffffu, 0);
}
#endif

/* ---- the calls ---- */

bool draw_init(void)
{
#ifdef PUZZLLM_NEURONTE
    if (font_state == 0) {
        snprintf(font_path, sizeof font_path, "%s", weights_path());
        font_state = neuronte_open_file(font_path, &font) == 0 ? 1 : -1;
        if (font_state < 0) font = NULL;
        else warm_cache();
    }
    return font_state > 0;
#else
    return false;
#endif
}

const char *draw_renderer(void)
{
#ifdef PUZZLLM_NEURONTE
    static char text[1100];
    if (nn()) {
        snprintf(text, sizeof text, "neuronte (%s)", font_path);
        return text;
    }
    if (font_state < 0) {
        snprintf(text, sizeof text, "raster (Neuronte weights did not open: %s)", font_path);
        return text;
    }
#endif
    return "raster";
}

void draw_force_raster(bool on)
{
    force_raster = on;
}

void draw_fill(sr_canvas *c, float x, float y, float w, float h, uint32_t rgb, float alpha)
{
#ifdef PUZZLLM_NEURONTE
    /* A bar no thicker than three pixels is a full block stretched to it. */
    if (nn()) {
        int iw = iround(w), ih = iround(h);
        if (iw >= 1 && ih >= 1 && (iw <= 3 || ih <= 3) && iw <= 64 && ih <= 64) {
            nn_scalar(c, 0x2588u, iround(x), iround(y), iw, ih, rgb, alpha, 0);
            return;
        }
    }
#endif
    plain_fill(c, x, y, w, h, rgb, alpha);
}

void draw_stroke(sr_canvas *c, float x, float y, float w, float h, float thickness,
                 uint32_t rgb, float alpha)
{
    int x0 = iround(x), y0 = iround(y);
    int iw = iround(x + w) - x0, ih = iround(y + h) - y0;
    int t = iround(thickness);
    if (t < 1) t = 1;
    if (iw < 2 * t || ih < 2 * t) {
        draw_fill(c, x, y, w, h, rgb, alpha);
        return;
    }
#ifdef PUZZLLM_NEURONTE
    /* An eighth block's ink is one eighth of its cell: a side t pixels
     * thick is that block in a cell 8t across, stretched along it. */
    if (nn()) {
        nn_scalar(c, 0x2594u, x0, y0, iw, 8 * t, rgb, alpha, 0);
        nn_scalar(c, 0x2581u, x0, y0 + ih - 8 * t, iw, 8 * t, rgb, alpha, 0);
        nn_scalar(c, 0x258fu, x0, y0 + t, 8 * t, ih - 2 * t, rgb, alpha, 0);
        nn_scalar(c, 0x2595u, x0 + iw - 8 * t, y0 + t, 8 * t, ih - 2 * t, rgb, alpha, 0);
        return;
    }
#endif
    {
        int a = alpha255(alpha);
        fill_box(c, x0, y0, x0 + iw, y0 + t, rgb, a);
        fill_box(c, x0, y0 + ih - t, x0 + iw, y0 + ih, rgb, a);
        fill_box(c, x0, y0 + t, x0 + t, y0 + ih - t, rgb, a);
        fill_box(c, x0 + iw - t, y0 + t, x0 + iw, y0 + ih - t, rgb, a);
    }
}

void draw_disc(sr_canvas *c, float cx, float cy, float rx, float ry, uint32_t rgb, float alpha)
{
#ifdef PUZZLLM_NEURONTE
    float box[4];
    if (nn() && rx > 0 && ry > 0 && disc_box(box)) {
        int cell_w = iround(2 * rx / (box[2] - box[0]));
        int cell_h = iround(2 * ry / (box[3] - box[1]));
        if (cell_w >= 1 && cell_h >= 1 && cell_w <= 2048 && cell_h <= 2048) {
            nn_scalar(c, 0x25cfu, iround(cx - rx - box[0] * (float)cell_w),
                      iround(cy - ry - box[1] * (float)cell_h), cell_w, cell_h, rgb, alpha, 0);
            return;
        }
    }
#endif
    raster_disc(c, cx, cy, rx, ry, rgb, alpha255(alpha));
}

void draw_text(sr_canvas *c, float x, float y, const char *s, uint32_t rgb, float alpha)
{
    size_t len, at = 0;
    int px = iround(x), py = iround(y), a = alpha255(alpha);
    if (c == NULL || s == NULL || a <= 0) return;
    len = strlen(s);
#ifdef PUZZLLM_NEURONTE
    if (nn()) {
        neuronte_draw_utf8(font, c->px, c->w, c->w, c->h, px, py, CW, CH, s, len,
                           nn_color(rgb, alpha));
        return;
    }
#endif
    while (at < len) {
        uint32_t cp;
        size_t n = utf8_next(s + at, len - at, &cp);
        if (n == 0) break;
        raster_glyph(c, px, py, cp, rgb & 0xffffffu, a, false);
        px += CW;
        at += n;
    }
}

int draw_text_width(const char *s)
{
    size_t len = s ? strlen(s) : 0, at = 0;
    int cells = 0;
    while (at < len) {
        uint32_t cp;
        size_t n = utf8_next(s + at, len - at, &cp);
        if (n == 0) break;
        at += n;
        cells++;
    }
    return cells * CW;
}

void draw_glyph(sr_canvas *c, int x, int y, int w, int h, uint32_t scalar,
                uint32_t rgb, unsigned style)
{
    if (c == NULL || scalar <= 0x20 || scalar == 0xa0) return;
#ifdef PUZZLLM_NEURONTE
    if (nn()) {
        unsigned neuronte_style = (style & DRAW_GLYPH_BOLD ? NEURONTE_STYLE_BOLD : 0) |
                                  (style & DRAW_GLYPH_ITALIC ? NEURONTE_STYLE_ITALIC : 0);
        nn_scalar(c, scalar, x, y, w, h, rgb, 1.0f, neuronte_style);
        return;
    }
#endif
    (void)w;
    (void)h;
    raster_glyph(c, x, y, scalar, rgb & 0xffffffu, 255, (style & DRAW_GLYPH_BOLD) != 0);
}

void draw_picture(sr_canvas *c, const uint32_t *px, int sw, int sh,
                  int x, int y, int w, int h, float alpha)
{
    int ga = alpha255(alpha);
    if (c == NULL || c->px == NULL || px == NULL || sw <= 0 || sh <= 0 ||
        w <= 0 || h <= 0 || ga <= 0 || c->px == px)
        return;
#ifdef PUZZLLM_NEURONTE
    if (nn() && nn_picture(c, px, sw, sh, x, y, w, h, ga)) return;
#endif
    raster_picture(c, px, sw, sh, x, y, w, h, ga);
}
