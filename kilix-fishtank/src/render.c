/* Game-specific drawing over the shared software rasterizer. */
#include "fishtank.h"
#include "embedded_assets.h"
#include "soft_raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define FISH_SWIM_FRAMES 8
#define FISH_TURN_FRAMES 5
#define SHARK_TURN_FRAMES 7

static sr_canvas canvas;
static uint8_t *fb = NULL;       /* repacked R,G,B,A presenter buffer */
static uint32_t *backdrop = NULL;
static int W = 0, H = 0;
static int backdropVersion = -1;

typedef struct {
    int w, h;
    int cx, cy;
    uint8_t *px;  /* premultiplied RGBA */
} Sprite;

typedef struct {
    int x, y, w, h;
    int cx, cy;
} AtlasFrame;

typedef struct {
    Sprite image;
    AtlasFrame frames[8];
    int frameCount;
} SpriteAtlas;

static Sprite fishSprites[FISH_SPECIES_COUNT][3][FISH_PALETTE_COUNT][FISH_SWIM_FRAMES];
static SpriteAtlas fishTurnAtlases[FISH_SPECIES_COUNT][3][FISH_PALETTE_COUNT];
static Sprite sharkSprites[6];
static SpriteAtlas sharkTurnAtlas;
static Sprite boatSprite;
static Sprite bubbleSprites[5];
static Sprite sandTileSprite;
static Sprite rockSprites[ENV_ROCK_ASSET_COUNT];
static Sprite plantSprites[ENV_PLANT_ASSET_COUNT];

static void build_sprites(void);
static void free_sprites(void);
static void build_backdrop(void);

uint8_t *render_fb(void) { return fb; }

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{ return sr_mix(a, b, t); }

static uint32_t scale_rgb(uint32_t rgb, float k)
{ return sr_scale_rgb(rgb, k); }

static float hash01(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x & 0xffffffu) * (1.0f / 16777216.0f);
}

void render_init(int w, int h)
{
    W = w;
    H = h;
    (void)sr_canvas_init(&canvas, W, H);
    fb = malloc((size_t)W * H * 4);
    backdrop = malloc((size_t)W * H * sizeof *backdrop);
    backdropVersion = -1;
    build_sprites();
}

void render_shutdown(void)
{
    free_sprites();
    sr_canvas_free(&canvas);
    free(fb);
    free(backdrop);
    fb = NULL;
    backdrop = NULL;
}

static inline void set_px(int x, int y, uint32_t rgb)
{
    sr_px(&canvas, x, y, rgb);
}

static inline void px_blend(int x, int y, uint32_t rgb, float a)
{
    sr_blend(&canvas, x, y, rgb, a);
}

/* Sprite assets remain in their generated premultiplied RGBA form.  These
 * two game-specific compositors bridge them into soft-raster's canvas. */
static inline void blend_sprite_pixel(int x, int y, const uint8_t *src,
                                      int globalAlpha, int sourceAlpha)
{
    uint32_t *pixel = &canvas.px[(size_t)y * W + x];
    int inv = 255 - sourceAlpha;
    int dr = (*pixel >> 16) & 255;
    int dg = (*pixel >> 8) & 255;
    int db = *pixel & 255;
    int r = src[0] * globalAlpha / 255 + dr * inv / 255;
    int g = src[1] * globalAlpha / 255 + dg * inv / 255;
    int b = src[2] * globalAlpha / 255 + db * inv / 255;
    *pixel = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 |
             (uint32_t)b;
}

static inline void blend_tint_pixel(int x, int y, int r, int g, int b,
                                    int sourceAlpha)
{
    uint32_t *pixel = &canvas.px[(size_t)y * W + x];
    int inv = 255 - sourceAlpha;
    int dr = (*pixel >> 16) & 255;
    int dg = (*pixel >> 8) & 255;
    int db = *pixel & 255;
    *pixel = 0xff000000u |
             (uint32_t)((r * sourceAlpha + dr * inv) / 255) << 16 |
             (uint32_t)((g * sourceAlpha + dg * inv) / 255) << 8 |
             (uint32_t)((b * sourceAlpha + db * inv) / 255);
}

static void fill_rect(float x, float y, float w, float h, uint32_t rgb, float a)
{ sr_fill_rect(&canvas, x, y, w, h, rgb, a); }

static void stroke_rect(float x, float y, float w, float h,
                        float line, uint32_t rgb, float a)
{ sr_stroke_rect(&canvas, x, y, w, h, line, rgb, a); }

static void fill_circle(float cx, float cy, float r, uint32_t rgb, float a)
{ sr_fill_circle(&canvas, cx, cy, r, rgb, a); }

static void ring(float cx, float cy, float r, float width, uint32_t rgb, float a)
{ sr_ring(&canvas, cx, cy, r, width, rgb, a); }

static void draw_line(float x0, float y0, float x1, float y1,
                      float width, uint32_t rgb, float a)
{ sr_line(&canvas, x0, y0, x1, y1, width, rgb, a, 0, 0); }
static void fill_ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, float a)
{
    if (rx <= 0.0f || ry <= 0.0f) return;
    int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1);
    int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
    float edge = fmaxf(rx, ry);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float nx = (x + 0.5f - cx) / rx;
            float ny = (y + 0.5f - cy) / ry;
            float d = sqrtf(nx * nx + ny * ny);
            if (d > 1.0f) continue;
            float cov = clampf((1.0f - d) * edge + 0.5f, 0.0f, 1.0f);
            px_blend(x, y, rgb, a * cov);
        }
    }
}

static void fill_triangle(float x0, float y0, float x1, float y1,
                          float x2, float y2, uint32_t rgb, float a)
{ sr_fill_triangle(&canvas, x0, y0, x1, y1, x2, y2, rgb, a); }

static void sprite_alloc(Sprite *sp, int w, int h, int cx, int cy)
{
    free(sp->px);
    sp->w = w;
    sp->h = h;
    sp->cx = cx;
    sp->cy = cy;
    sp->px = calloc((size_t)w * h * 4, 1);
}

static void sprite_free(Sprite *sp)
{
    free(sp->px);
    memset(sp, 0, sizeof *sp);
}

static void atlas_free(SpriteAtlas *atlas)
{
    sprite_free(&atlas->image);
    memset(atlas->frames, 0, sizeof atlas->frames);
    atlas->frameCount = 0;
}

static inline void spr_px(Sprite *sp, int x, int y, uint32_t rgb, float a)
{
    if (!sp->px || x < 0 || x >= sp->w || y < 0 || y >= sp->h) return;
    int sa = (int)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (sa <= 0) return;
    int inv = 255 - sa;
    uint8_t *p = sp->px + ((size_t)y * sp->w + x) * 4;
    int r = (rgb >> 16) & 255;
    int g = (rgb >> 8) & 255;
    int b = rgb & 255;
    p[0] = (uint8_t)((r * sa + p[0] * inv) / 255);
    p[1] = (uint8_t)((g * sa + p[1] * inv) / 255);
    p[2] = (uint8_t)((b * sa + p[2] * inv) / 255);
    p[3] = (uint8_t)(sa + (p[3] * inv) / 255);
}

static void spr_fill_circle(Sprite *sp, float cx, float cy, float r, uint32_t rgb, float a)
{
    if (r <= 0.0f) return;
    float ro = r + 0.5f;
    float ri = r - 0.5f;
    float ro2 = ro * ro;
    float ri2 = ri > 0.0f ? ri * ri : 0.0f;
    int y0 = (int)floorf(cy - ro), y1 = (int)ceilf(cy + ro);
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        float w2 = ro2 - dy * dy;
        if (w2 <= 0.0f) continue;
        float half = sqrtf(w2);
        int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= ro2) continue;
            float cov = d2 <= ri2 ? 1.0f : clampf(ro - sqrtf(d2), 0.0f, 1.0f);
            spr_px(sp, x, y, rgb, a * cov);
        }
    }
}

static float spr_edge_fn(float ax, float ay, float bx, float by,
                         float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void spr_triangle(Sprite *sp, float x0, float y0, float x1, float y1,
                         float x2, float y2, uint32_t rgb, float a)
{
    int minX = (int)floorf(fminf(x0, fminf(x1, x2))) - 1;
    int maxX = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int minY = (int)floorf(fminf(y0, fminf(y1, y2))) - 1;
    int maxY = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float py = y + 0.5f;
            float e0 = spr_edge_fn(x0, y0, x1, y1, px, py);
            float e1 = spr_edge_fn(x1, y1, x2, y2, px, py);
            float e2 = spr_edge_fn(x2, y2, x0, y0, px, py);
            bool neg = e0 < 0.0f || e1 < 0.0f || e2 < 0.0f;
            bool pos = e0 > 0.0f || e1 > 0.0f || e2 > 0.0f;
            if (!(neg && pos))
                spr_px(sp, x, y, rgb, a);
        }
    }
}

static void spr_line(Sprite *sp, float x0, float y0, float x1, float y1,
                     float width, uint32_t rgb, float a)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    if (len2 < 0.1f) {
        spr_fill_circle(sp, x0, y0, width * 0.5f, rgb, a);
        return;
    }
    float hw = fmaxf(0.5f, width * 0.5f);
    int xMin = (int)floorf(fminf(x0, x1) - hw) - 1;
    int xMax = (int)ceilf(fmaxf(x0, x1) + hw) + 1;
    int yMin = (int)floorf(fminf(y0, y1) - hw) - 1;
    int yMax = (int)ceilf(fmaxf(y0, y1) + hw) + 1;
    for (int y = yMin; y < yMax; y++) {
        for (int x = xMin; x < xMax; x++) {
            float px = x + 0.5f - x0;
            float py = y + 0.5f - y0;
            float t = clampf((px * dx + py * dy) / len2, 0.0f, 1.0f);
            float qx = px - t * dx;
            float qy = py - t * dy;
            float cov = hw + 0.5f - sqrtf(qx * qx + qy * qy);
            if (cov > 0.0f)
                spr_px(sp, x, y, rgb, a * clampf(cov, 0.0f, 1.0f));
        }
    }
}

static void spr_ring(Sprite *sp, float cx, float cy, float r, float width,
                     uint32_t rgb, float a)
{
    float hw = fmaxf(0.45f, width * 0.5f);
    int x0 = (int)floorf(cx - r - hw) - 1;
    int x1 = (int)ceilf(cx + r + hw) + 1;
    int y0 = (int)floorf(cy - r - hw) - 1;
    int y1 = (int)ceilf(cy + r + hw) + 1;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx;
            float dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = hw + 0.5f - fabsf(d - r);
            if (cov > 0.0f)
                spr_px(sp, x, y, rgb, a * clampf(cov, 0.0f, 1.0f));
        }
    }
}

static void blit_sprite(const Sprite *sp, float cx, float cy, bool flip, float alpha)
{
    if (!sp->px || alpha <= 0.0f) return;
    int x0 = (int)lroundf(cx) - sp->cx;
    int y0 = (int)lroundf(cy) - sp->cy;
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (int y = 0; y < sp->h; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= H) continue;
        for (int x = 0; x < sp->w; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= W) continue;
            int sx = flip ? sp->w - 1 - x : x;
            const uint8_t *src = sp->px + ((size_t)y * sp->w + sx) * 4;
            int sa = (src[3] * ga) / 255;
            if (sa <= 0) continue;
            blend_sprite_pixel(dx, dy, src, ga, sa);
        }
    }
}

static void blit_sprite_tint(const Sprite *sp, float cx, float cy, bool flip,
                             uint32_t rgb, float alpha, float expand)
{
    if (!sp->px || alpha <= 0.0f) return;
    int x0 = (int)lroundf(cx) - sp->cx;
    int y0 = (int)lroundf(cy) - sp->cy;
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int tr = (rgb >> 16) & 255;
    int tg = (rgb >> 8) & 255;
    int tb = rgb & 255;
    int radius = (int)ceilf(expand);
    if (radius < 0) radius = 0;
    for (int y = 0; y < sp->h; y++) {
        int dy = y0 + y;
        if (dy < -radius || dy >= H + radius) continue;
        for (int x = 0; x < sp->w; x++) {
            int sx = flip ? sp->w - 1 - x : x;
            const uint8_t *src = sp->px + ((size_t)y * sp->w + sx) * 4;
            int baseA = (src[3] * ga) / 255;
            if (baseA <= 0) continue;
            for (int oy = -radius; oy <= radius; oy++) {
                int yy = dy + oy;
                if (yy < 0 || yy >= H) continue;
                for (int ox = -radius; ox <= radius; ox++) {
                    int xx = x0 + x + ox;
                    if (xx < 0 || xx >= W) continue;
                    int fade = radius > 0 ? 255 - (int)(sqrtf((float)(ox * ox + oy * oy)) * 110.0f) : 255;
                    if (fade <= 0) continue;
                    int sa = (baseA * fade) / 255;
                    blend_tint_pixel(xx, yy, tr, tg, tb, sa);
                }
            }
        }
    }
}

static void blit_sprite_scaled(const Sprite *sp, float cx, float cy, bool flip,
                               float alpha, float sxScale, float syScale)
{
    if (!sp->px || alpha <= 0.0f) return;
    sxScale = fmaxf(0.08f, sxScale);
    syScale = fmaxf(0.08f, syScale);
    int dw = (int)ceilf(sp->w * sxScale);
    int dh = (int)ceilf(sp->h * syScale);
    int x0 = (int)lroundf(cx - sp->cx * sxScale);
    int y0 = (int)lroundf(cy - sp->cy * syScale);
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (int y = 0; y < dh; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= H) continue;
        int sy = (int)clampf(floorf((y + 0.5f) / syScale), 0.0f, (float)(sp->h - 1));
        for (int x = 0; x < dw; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= W) continue;
            int sx = (int)clampf(floorf((x + 0.5f) / sxScale), 0.0f, (float)(sp->w - 1));
            if (flip) sx = sp->w - 1 - sx;
            const uint8_t *src = sp->px + ((size_t)sy * sp->w + sx) * 4;
            int sa = (src[3] * ga) / 255;
            if (sa <= 0) continue;
            blend_sprite_pixel(dx, dy, src, ga, sa);
        }
    }
}

static void blit_sprite_tint_scaled(const Sprite *sp, float cx, float cy, bool flip,
                                    uint32_t rgb, float alpha, float expand,
                                    float sxScale, float syScale)
{
    if (!sp->px || alpha <= 0.0f) return;
    sxScale = fmaxf(0.08f, sxScale);
    syScale = fmaxf(0.08f, syScale);
    int dw = (int)ceilf(sp->w * sxScale);
    int dh = (int)ceilf(sp->h * syScale);
    int x0 = (int)lroundf(cx - sp->cx * sxScale);
    int y0 = (int)lroundf(cy - sp->cy * syScale);
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int tr = (rgb >> 16) & 255;
    int tg = (rgb >> 8) & 255;
    int tb = rgb & 255;
    int radius = (int)ceilf(expand);
    if (radius < 0) radius = 0;
    for (int y = 0; y < dh; y++) {
        int dy = y0 + y;
        if (dy < -radius || dy >= H + radius) continue;
        int sy = (int)clampf(floorf((y + 0.5f) / syScale), 0.0f, (float)(sp->h - 1));
        for (int x = 0; x < dw; x++) {
            int sx = (int)clampf(floorf((x + 0.5f) / sxScale), 0.0f, (float)(sp->w - 1));
            if (flip) sx = sp->w - 1 - sx;
            const uint8_t *src = sp->px + ((size_t)sy * sp->w + sx) * 4;
            int baseA = (src[3] * ga) / 255;
            if (baseA <= 0) continue;
            for (int oy = -radius; oy <= radius; oy++) {
                int yy = dy + oy;
                if (yy < 0 || yy >= H) continue;
                for (int ox = -radius; ox <= radius; ox++) {
                    int xx = x0 + x + ox;
                    if (xx < 0 || xx >= W) continue;
                    int fade = radius > 0 ? 255 - (int)(sqrtf((float)(ox * ox + oy * oy)) * 110.0f) : 255;
                    if (fade <= 0) continue;
                    int sa = (baseA * fade) / 255;
                    blend_tint_pixel(xx, yy, tr, tg, tb, sa);
                }
            }
        }
    }
}

static const AtlasFrame *atlas_frame(const SpriteAtlas *atlas, int frame)
{
    if (!atlas->image.px || atlas->frameCount <= 0) return NULL;
    if (frame < 0) frame = 0;
    if (frame >= atlas->frameCount) frame = atlas->frameCount - 1;
    return &atlas->frames[frame];
}

static void blit_atlas_scaled(const SpriteAtlas *atlas, int frame,
                              float cx, float cy, bool flip,
                              float alpha, float sxScale, float syScale)
{
    const AtlasFrame *fr = atlas_frame(atlas, frame);
    if (!fr || alpha <= 0.0f) return;
    sxScale = fmaxf(0.08f, sxScale);
    syScale = fmaxf(0.08f, syScale);
    int dw = (int)ceilf(fr->w * sxScale);
    int dh = (int)ceilf(fr->h * syScale);
    int x0 = (int)lroundf(cx - fr->cx * sxScale);
    int y0 = (int)lroundf(cy - fr->cy * syScale);
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    const Sprite *img = &atlas->image;
    for (int y = 0; y < dh; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= H) continue;
        int sy = (int)clampf(floorf((y + 0.5f) / syScale), 0.0f, (float)(fr->h - 1));
        for (int x = 0; x < dw; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= W) continue;
            int sx = (int)clampf(floorf((x + 0.5f) / sxScale), 0.0f, (float)(fr->w - 1));
            if (flip) sx = fr->w - 1 - sx;
            const uint8_t *src = img->px + ((size_t)(fr->y + sy) * img->w + fr->x + sx) * 4;
            int sa = (src[3] * ga) / 255;
            if (sa <= 0) continue;
            blend_sprite_pixel(dx, dy, src, ga, sa);
        }
    }
}

static void blit_atlas_tint_scaled(const SpriteAtlas *atlas, int frame,
                                   float cx, float cy, bool flip,
                                   uint32_t rgb, float alpha, float expand,
                                   float sxScale, float syScale)
{
    const AtlasFrame *fr = atlas_frame(atlas, frame);
    if (!fr || alpha <= 0.0f) return;
    sxScale = fmaxf(0.08f, sxScale);
    syScale = fmaxf(0.08f, syScale);
    int dw = (int)ceilf(fr->w * sxScale);
    int dh = (int)ceilf(fr->h * syScale);
    int x0 = (int)lroundf(cx - fr->cx * sxScale);
    int y0 = (int)lroundf(cy - fr->cy * syScale);
    int ga = (int)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int tr = (rgb >> 16) & 255;
    int tg = (rgb >> 8) & 255;
    int tb = rgb & 255;
    int radius = (int)ceilf(expand);
    if (radius < 0) radius = 0;
    const Sprite *img = &atlas->image;
    for (int y = 0; y < dh; y++) {
        int dy = y0 + y;
        if (dy < -radius || dy >= H + radius) continue;
        int sy = (int)clampf(floorf((y + 0.5f) / syScale), 0.0f, (float)(fr->h - 1));
        for (int x = 0; x < dw; x++) {
            int sx = (int)clampf(floorf((x + 0.5f) / sxScale), 0.0f, (float)(fr->w - 1));
            if (flip) sx = fr->w - 1 - sx;
            const uint8_t *src = img->px + ((size_t)(fr->y + sy) * img->w + fr->x + sx) * 4;
            int baseA = (src[3] * ga) / 255;
            if (baseA <= 0) continue;
            for (int oy = -radius; oy <= radius; oy++) {
                int yy = dy + oy;
                if (yy < 0 || yy >= H) continue;
                for (int ox = -radius; ox <= radius; ox++) {
                    int xx = x0 + x + ox;
                    if (xx < 0 || xx >= W) continue;
                    int fade = radius > 0 ? 255 - (int)(sqrtf((float)(ox * ox + oy * oy)) * 110.0f) : 255;
                    if (fade <= 0) continue;
                    int sa = (baseA * fade) / 255;
                    blend_tint_pixel(xx, yy, tr, tg, tb, sa);
                }
            }
        }
    }
}

static int fish_tier(float size)
{
    if (size < 1.18f) return 0;
    if (size < 1.72f) return 1;
    return 2;
}

static inline uint8_t asset_chan(const unsigned char *src, int sw, int x, int y, int c)
{
    return src[((size_t)y * sw + x) * 4 + c];
}

static void make_raw_asset_atlas(SpriteAtlas *atlas, const unsigned char *src,
                                 int sw, int sh, int frameCount,
                                 float cxFrac, float cyFrac)
{
    if (frameCount <= 0) return;
    atlas_free(atlas);
    sprite_alloc(&atlas->image, sw, sh, 0, 0);
    atlas->frameCount = frameCount;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int sr = asset_chan(src, sw, x, y, 0);
            int sg = asset_chan(src, sw, x, y, 1);
            int sb = asset_chan(src, sw, x, y, 2);
            int sa = asset_chan(src, sw, x, y, 3);
            uint8_t *p = atlas->image.px + ((size_t)y * sw + x) * 4;
            p[0] = (uint8_t)((sr * sa) / 255);
            p[1] = (uint8_t)((sg * sa) / 255);
            p[2] = (uint8_t)((sb * sa) / 255);
            p[3] = (uint8_t)sa;
        }
    }

    int cellW = sw / frameCount;
    for (int i = 0; i < frameCount; i++) {
        atlas->frames[i] = (AtlasFrame){
            .x = i * cellW, .y = 0, .w = cellW, .h = sh,
            .cx = (int)lroundf(cellW * cxFrac),
            .cy = (int)lroundf(sh * cyFrac)
        };
    }
}

typedef struct {
    const unsigned char *rgba;
    int w, h;
    bool tint;
    float widthBase;
    float widthStep;
    float anchorY;
    const unsigned char *turnRgba;
    int turnW, turnH;
} FishAsset;

static FishAsset fish_asset_for_species(int species)
{
    switch (species) {
    case 0:
        return (FishAsset){ ASSET_CLOWNFISH_RGBA, ASSET_CLOWNFISH_W, ASSET_CLOWNFISH_H,
                            false, 24.0f, 8.0f, 0.54f,
                            ASSET_TURN_CLOWNFISH_RGBA, ASSET_TURN_CLOWNFISH_W, ASSET_TURN_CLOWNFISH_H };
    case 1:
        return (FishAsset){ ASSET_BLUE_TANG_RGBA, ASSET_BLUE_TANG_W, ASSET_BLUE_TANG_H,
                            false, 31.0f, 10.0f, 0.54f,
                            ASSET_TURN_BLUE_TANG_RGBA, ASSET_TURN_BLUE_TANG_W, ASSET_TURN_BLUE_TANG_H };
    case 2:
        return (FishAsset){ ASSET_YELLOW_TANG_RGBA, ASSET_YELLOW_TANG_W, ASSET_YELLOW_TANG_H,
                            false, 30.0f, 10.0f, 0.54f,
                            ASSET_TURN_YELLOW_TANG_RGBA, ASSET_TURN_YELLOW_TANG_W, ASSET_TURN_YELLOW_TANG_H };
    case 3:
        return (FishAsset){ ASSET_COPPERBAND_RGBA, ASSET_COPPERBAND_W, ASSET_COPPERBAND_H,
                            false, 32.0f, 11.0f, 0.52f,
                            ASSET_TURN_COPPERBAND_RGBA, ASSET_TURN_COPPERBAND_W, ASSET_TURN_COPPERBAND_H };
    case 4:
        return (FishAsset){ ASSET_LIONFISH_RGBA, ASSET_LIONFISH_W, ASSET_LIONFISH_H,
                            false, 35.0f, 11.0f, 0.53f,
                            ASSET_TURN_LIONFISH_RGBA, ASSET_TURN_LIONFISH_W, ASSET_TURN_LIONFISH_H };
    case 5:
        return (FishAsset){ ASSET_MANDARIN_RGBA, ASSET_MANDARIN_W, ASSET_MANDARIN_H,
                            false, 25.0f, 8.0f, 0.54f,
                            ASSET_TURN_MANDARIN_RGBA, ASSET_TURN_MANDARIN_W, ASSET_TURN_MANDARIN_H };
    case 6:
        return (FishAsset){ ASSET_MOORISH_IDOL_RGBA, ASSET_MOORISH_IDOL_W, ASSET_MOORISH_IDOL_H,
                            false, 32.0f, 11.0f, 0.52f,
                            ASSET_TURN_MOORISH_IDOL_RGBA, ASSET_TURN_MOORISH_IDOL_W, ASSET_TURN_MOORISH_IDOL_H };
    case 7:
        return (FishAsset){ ASSET_ROYAL_ANGEL_RGBA, ASSET_ROYAL_ANGEL_W, ASSET_ROYAL_ANGEL_H,
                            false, 34.0f, 12.0f, 0.54f,
                            ASSET_TURN_ROYAL_ANGEL_RGBA, ASSET_TURN_ROYAL_ANGEL_W, ASSET_TURN_ROYAL_ANGEL_H };
    default:
        return fish_asset_for_species(0);
    }
}

static void make_asset_sprite(Sprite *sp, const unsigned char *src, int sw, int sh,
                              int targetW, int targetH, float cxFrac, float cyFrac,
                              bool tintFish, int palette, int frame, int kind)
{
    if (targetW < 2) targetW = 2;
    if (targetH < 2) targetH = 2;
    sprite_alloc(sp, targetW, targetH,
                 (int)lroundf(targetW * cxFrac),
                 (int)lroundf(targetH * cyFrac));

    const FishPalette *pal = &FISH_PALETTES[palette % FISH_PALETTE_COUNT];
    float phase = (float)(frame & 3) * PI * 0.5f;
    for (int y = 0; y < targetH; y++) {
        for (int x = 0; x < targetW; x++) {
            float u = (x + 0.5f) / (float)targetW;
            float v = (y + 0.5f) / (float)targetH;
            float warp = 0.0f;
            if (kind == 1)
                warp = sinf(phase) * fmaxf(0.0f, (0.38f - u) / 0.38f) * 0.060f;
            else if (kind == 2)
                warp = sinf(phase) * fmaxf(0.0f, (0.32f - u) / 0.32f) * 0.045f;
            float vv = clampf(v + warp, 0.0f, 0.999f);
            int sx = (int)clampf(floorf(u * sw), 0.0f, (float)(sw - 1));
            int sy = (int)clampf(floorf(vv * sh), 0.0f, (float)(sh - 1));
            int sr = asset_chan(src, sw, sx, sy, 0);
            int sg = asset_chan(src, sw, sx, sy, 1);
            int sb = asset_chan(src, sw, sx, sy, 2);
            int sa = asset_chan(src, sw, sx, sy, 3);
            if (sa <= 0) continue;

            int r = sr, g = sg, b = sb;
            if (tintFish) {
                int lum = (sr * 77 + sg * 150 + sb * 29) >> 8;
                if (lum > 42) {
                    float l = lum / 255.0f;
                    uint32_t base = mix_rgb(pal->body, pal->fin, clampf((u < 0.34f || v > 0.58f) ? 0.45f : 0.12f, 0, 1));
                    uint32_t lit = mix_rgb(scale_rgb(base, 0.45f + l * 0.95f), 0xffffff,
                                           clampf((l - 0.70f) / 0.30f, 0.0f, 1.0f) * 0.70f);
                    r = (lit >> 16) & 255;
                    g = (lit >> 8) & 255;
                    b = lit & 255;
                }
            }

            uint8_t *p = sp->px + ((size_t)y * sp->w + x) * 4;
            p[0] = (uint8_t)((r * sa) / 255);
            p[1] = (uint8_t)((g * sa) / 255);
            p[2] = (uint8_t)((b * sa) / 255);
            p[3] = (uint8_t)sa;
        }
    }
}

static void make_prop_sprite(Sprite *sp, const unsigned char *src, int sw, int sh,
                             float targetWUnits, float cxFrac, float cyFrac)
{
    int tw = (int)lroundf(targetWUnits * G.scale);
    int th = (int)lroundf(tw * (sh / (float)sw));
    make_asset_sprite(sp, src, sw, sh, tw, th, cxFrac, cyFrac, false, 0, 0, 0);
}

static void make_fish_sprite(Sprite *sp, int species, int tier, int palette, int frame)
{
    FishAsset asset = fish_asset_for_species(species);
    bool assetAttack = frame >= 4;
    int tw = (int)lroundf((asset.widthBase + tier * asset.widthStep) * G.scale *
                          (assetAttack ? 1.07f : 1.0f));
    int th = (int)lroundf(tw * (asset.h / (float)asset.w));
    make_asset_sprite(sp, asset.rgba, asset.w, asset.h,
                      tw, th, 0.50f, asset.anchorY, asset.tint, palette, frame, 1);
    if (assetAttack) {
        float s = G.scale;
        float mx = sp->w * 0.86f;
        float my = sp->h * 0.54f;
        spr_line(sp, mx - 2.0f * s, my, mx + 8.0f * s, my + 2.5f * s,
                 fmaxf(1.0f, 1.3f * s), 0x3f0b0b, 0.85f);
        for (int i = 0; i < 3; i++) {
            float tx = mx + i * 2.7f * s;
            spr_triangle(sp, tx, my - 0.2f * s, tx + 1.4f * s, my - 0.2f * s,
                         tx + 0.7f * s, my + 2.4f * s, 0xffffff, 0.85f);
        }
    }
}

static void make_shark_sprite(Sprite *sp, int frame)
{
    bool assetSharkAttack = frame >= 3;
    int tw = (int)lroundf(140.0f * G.scale * (assetSharkAttack ? 1.04f : 1.0f));
    int th = (int)lroundf(tw * (ASSET_SHARK_H / (float)ASSET_SHARK_W));
    make_asset_sprite(sp, ASSET_SHARK_RGBA, ASSET_SHARK_W, ASSET_SHARK_H,
                      tw, th, 0.50f, 0.53f, false, 0, frame, 2);
    if (assetSharkAttack) {
        float s = G.scale;
        float mx = sp->w * 0.86f;
        float my = sp->h * 0.58f;
        spr_line(sp, mx - 4.0f * s, my, mx + 10.0f * s, my + 3.0f * s,
                 fmaxf(1.0f, 1.7f * s), 0x3b0a0a, 0.90f);
        for (int i = 0; i < 5; i++) {
            float tx = mx - 2.0f * s + i * 2.8f * s;
            spr_triangle(sp, tx, my - 0.4f * s, tx + 1.7f * s, my - 0.4f * s,
                         tx + 0.85f * s, my + 3.2f * s, 0xffffff, 0.95f);
        }
    }
}

static void make_bubble_sprite(Sprite *sp, int tier)
{
    float s = G.scale;
    float r = (1.8f + tier * 1.1f) * s;
    int dim = (int)ceilf(r * 2.8f + 4.0f);
    sprite_alloc(sp, dim, dim, dim / 2, dim / 2);
    float c = dim * 0.5f;
    spr_ring(sp, c, c, r, fmaxf(1.0f, r * 0.34f), 0xc9f2ff, 0.55f);
    spr_fill_circle(sp, c - r * 0.35f, c - r * 0.35f,
                    fmaxf(0.75f, r * 0.22f), 0xffffff, 0.48f);
}

static void build_sprites(void)
{
    for (int spc = 0; spc < FISH_SPECIES_COUNT; spc++) {
        int paletteCount = fish_asset_for_species(spc).tint ? FISH_PALETTE_COUNT : 1;
        for (int t = 0; t < 3; t++)
            for (int p = 0; p < paletteCount; p++) {
                for (int f = 0; f < FISH_SWIM_FRAMES; f++)
                    make_fish_sprite(&fishSprites[spc][t][p][f], spc, t, p, f);
                FishAsset asset = fish_asset_for_species(spc);
                make_raw_asset_atlas(&fishTurnAtlases[spc][t][p],
                                     asset.turnRgba, asset.turnW, asset.turnH,
                                     FISH_TURN_FRAMES, 0.50f, asset.anchorY);
            }
    }
    for (int f = 0; f < 6; f++)
        make_shark_sprite(&sharkSprites[f], f);
    make_raw_asset_atlas(&sharkTurnAtlas, ASSET_TURN_SHARK_RGBA,
                         ASSET_TURN_SHARK_W, ASSET_TURN_SHARK_H,
                         SHARK_TURN_FRAMES, 0.50f, 0.53f);
    int boatH = (int)lroundf(132.0f * G.scale);
    int boatW = (int)lroundf(boatH * (ASSET_BOAT_W / (float)ASSET_BOAT_H));
    make_asset_sprite(&boatSprite, ASSET_BOAT_RGBA, ASSET_BOAT_W, ASSET_BOAT_H,
                      boatW, boatH, 0.50f, 0.86f, false, 0, 0, 0);
    for (int b = 0; b < 5; b++)
        make_bubble_sprite(&bubbleSprites[b], b);

    int sandW = (int)lroundf(260.0f * G.scale);
    int sandH = (int)lroundf(sandW * (ASSET_SAND_TILE_H / (float)ASSET_SAND_TILE_W));
    make_asset_sprite(&sandTileSprite, ASSET_SAND_TILE_RGBA, ASSET_SAND_TILE_W, ASSET_SAND_TILE_H,
                      sandW, sandH, 0.0f, 0.0f, false, 0, 0, 0);

    make_prop_sprite(&rockSprites[0], ASSET_ROCK_CLUSTER_RGBA,
                     ASSET_ROCK_CLUSTER_W, ASSET_ROCK_CLUSTER_H, 86.0f, 0.50f, 0.86f);
    make_prop_sprite(&rockSprites[1], ASSET_ROCK_SLATE_RGBA,
                     ASSET_ROCK_SLATE_W, ASSET_ROCK_SLATE_H, 98.0f, 0.50f, 0.88f);
    make_prop_sprite(&rockSprites[2], ASSET_ROCK_REEF_RGBA,
                     ASSET_ROCK_REEF_W, ASSET_ROCK_REEF_H, 106.0f, 0.50f, 0.88f);

    make_prop_sprite(&plantSprites[0], ASSET_PLANT_SEAWEED_RGBA,
                     ASSET_PLANT_SEAWEED_W, ASSET_PLANT_SEAWEED_H, 45.0f, 0.50f, 0.96f);
    make_prop_sprite(&plantSprites[1], ASSET_PLANT_CORAL_RGBA,
                     ASSET_PLANT_CORAL_W, ASSET_PLANT_CORAL_H, 74.0f, 0.50f, 0.94f);
    make_prop_sprite(&plantSprites[2], ASSET_PLANT_ANEMONE_RGBA,
                     ASSET_PLANT_ANEMONE_W, ASSET_PLANT_ANEMONE_H, 70.0f, 0.50f, 0.88f);
    make_prop_sprite(&plantSprites[3], ASSET_PLANT_SPONGE_RGBA,
                     ASSET_PLANT_SPONGE_W, ASSET_PLANT_SPONGE_H, 78.0f, 0.50f, 0.94f);
}

static void free_sprites(void)
{
    for (int spc = 0; spc < FISH_SPECIES_COUNT; spc++)
        for (int t = 0; t < 3; t++)
            for (int p = 0; p < FISH_PALETTE_COUNT; p++) {
                for (int f = 0; f < FISH_SWIM_FRAMES; f++)
                    sprite_free(&fishSprites[spc][t][p][f]);
                atlas_free(&fishTurnAtlases[spc][t][p]);
            }
    for (int f = 0; f < 6; f++)
        sprite_free(&sharkSprites[f]);
    atlas_free(&sharkTurnAtlas);
    sprite_free(&boatSprite);
    for (int b = 0; b < 5; b++)
        sprite_free(&bubbleSprites[b]);
    sprite_free(&sandTileSprite);
    for (int r = 0; r < ENV_ROCK_ASSET_COUNT; r++)
        sprite_free(&rockSprites[r]);
    for (int p = 0; p < ENV_PLANT_ASSET_COUNT; p++)
        sprite_free(&plantSprites[p]);
}

static int text_width(const char *s, int scale)
{ return sr_text_width(s, scale); }

static void draw_text(float x, float y, const char *s,
                      uint32_t rgb, float a, int scale)
{ sr_text(&canvas, x, y, s, rgb, a, scale); }

static void draw_text_shadow(float x, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{
    sr_text(&canvas, x + scale, y + scale, s, 0x001018, a * 0.82f, scale);
    sr_text(&canvas, x, y, s, rgb, a, scale);
}

static void draw_text_center(float cx, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{ sr_text_center(&canvas, cx, y, s, rgb, a, scale); }

static void draw_treasure_chest(float x, float y)
{
    float s = G.scale;
    fill_ellipse(x + 2.0f * s, y + 20.0f * s, 34.0f * s, 6.0f * s, 0x000a10, 0.28f);
    fill_rect(x - 26.0f * s, y, 52.0f * s, 21.0f * s, 0x6b3f1d, 1.0f);
    fill_rect(x - 22.0f * s, y + 3.0f * s, 44.0f * s, 7.0f * s, 0xb26b2b, 0.88f);
    fill_rect(x - 28.0f * s, y - 2.0f * s, 56.0f * s, 5.0f * s, 0xd4a85f, 0.85f);
    stroke_rect(x - 26.0f * s, y, 52.0f * s, 21.0f * s, 2.0f * s, 0x2a1609, 0.70f);
    fill_rect(x - 3.5f * s, y + 6.0f * s, 7.0f * s, 8.0f * s, 0xfacc15, 0.95f);
    for (int i = -2; i <= 2; i++)
        fill_circle(x + i * 7.0f * s, y - 4.0f * s + fabsf((float)i) * 1.2f * s,
                    2.1f * s, 0xfff1a8, 0.55f);
}

static void draw_ruins(float x, float base)
{
    float s = G.scale;
    uint32_t stone = 0x7b8794;
    for (int i = 0; i < 4; i++) {
        float h = (34.0f + i * 9.0f) * s;
        float w = (12.0f - i * 0.9f) * s;
        float px = x + (i * 20.0f - 31.0f) * s;
        fill_rect(px, base - h, w, h, scale_rgb(stone, 0.72f + i * 0.06f), 0.78f);
        fill_rect(px - 2.0f * s, base - h - 3.0f * s, w + 4.0f * s, 4.0f * s,
                  scale_rgb(stone, 0.95f), 0.78f);
    }
    draw_line(x - 42.0f * s, base - 41.0f * s, x + 49.0f * s, base - 26.0f * s,
              5.0f * s, scale_rgb(stone, 0.62f), 0.66f);
}

static void draw_backdrop_decor(void)
{
    float s = G.scale;
    float base = (float)G.groundY;
    fill_rect(0, base - 14.0f * s, W, 14.0f * s, 0x041827, 0.16f);

    for (int i = 0; i < 8; i++) {
        const Sprite *sp = &plantSprites[i % ENV_PLANT_ASSET_COUNT];
        float x = W * (0.055f + i * 0.128f + (hash01(400u + i) - 0.5f) * 0.040f);
        float scale = 0.55f + hash01(500u + i) * 0.58f;
        float y = base + (2.0f + hash01(650u + i) * 5.0f) * s;
        blit_sprite_scaled(sp, x, y, (i & 1) != 0, 0.24f, scale, scale);
    }

    for (int i = 0; i < 5; i++) {
        const Sprite *sp = &rockSprites[i % ENV_ROCK_ASSET_COUNT];
        float x = W * (0.10f + i * 0.205f + (hash01(900u + i) - 0.5f) * 0.045f);
        float y = base + (7.0f + hash01(950u + i) * 8.0f) * s;
        float scale = 0.50f + hash01(980u + i) * 0.42f;
        blit_sprite_scaled(sp, x, y, (i & 1) != 0, 0.22f, scale, scale);
    }

    draw_ruins(W * 0.18f, base + 1.0f * s);
    draw_treasure_chest(W * 0.79f, base - 20.0f * s);
}

static void draw_sand_bed(int ground)
{
    const Sprite *tile = &sandTileSprite;
    if (!tile->px || tile->w <= 0 || tile->h <= 0) {
        fill_rect(0, ground, W, H - ground, 0xc2b280, 1.0f);
        return;
    }

    int strideX = tile->w - 1;
    int strideY = tile->h - 1;
    if (strideX < 32) strideX = tile->w;
    if (strideY < 24) strideY = tile->h;

    int offset = (G.worldVersion * 37) % strideX;
    for (int y = ground; y < H + tile->h; y += strideY) {
        for (int x = -offset; x < W + tile->w; x += strideX) {
            bool flip = (((x / strideX) + (y / strideY)) & 1) != 0;
            blit_sprite(tile, (float)x, (float)y, flip, 1.0f);
        }
    }

    int depth = H - ground;
    for (int i = 0; i < 5; i++) {
        float y0 = ground + depth * (i / 5.0f);
        float y1 = ground + depth * ((i + 1) / 5.0f);
        fill_rect(0.0f, y0, (float)W, y1 - y0, 0x312514, 0.020f + i * 0.024f);
    }
}

static void draw_background(void)
{
    uint32_t top = 0x164b90;
    uint32_t mid = 0x0b5f7a;
    uint32_t bot = 0x07213f;
    int ground = G.groundY;
    int water = G.waterY;
    if (ground < 1) ground = H - 1;
    if (water < 0) water = 0;
    if (water > ground - 1) water = ground - 1;

    for (int y = 0; y < water; y++) {
        float t = (float)y / fmaxf(1.0f, (float)(water - 1));
        uint32_t c = mix_rgb(0x050b12, 0x0b1c2a, t);
        uint32_t *row = canvas.px + (size_t)y * W;
        for (int x = 0; x < W; x++) row[x] = 0xff000000u | c;
    }

    for (int y = water; y < ground; y++) {
        float t = (float)(y - water) / fmaxf(1.0f, (float)(ground - water - 1));
        uint32_t c = t < 0.45f ? mix_rgb(top, mid, t / 0.45f)
                               : mix_rgb(mid, bot, (t - 0.45f) / 0.55f);
        int r = (c >> 16) & 255;
        int g = (c >> 8) & 255;
        int b = c & 255;
        uint32_t *row = canvas.px + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            float edge = fabsf((float)x / fmaxf(1.0f, W - 1.0f) - 0.5f) * 2.0f;
            float shade = 1.0f - 0.19f * edge * edge - 0.08f * t;
            row[x] = 0xff000000u |
                     (uint32_t)(uint8_t)(r * shade) << 16 |
                     (uint32_t)(uint8_t)(g * shade) << 8 |
                     (uint32_t)(uint8_t)(b * shade);
        }
    }

    for (int i = 0; i < 6; i++) {
        float sx = (W * (0.10f + i * 0.17f));
        float w = W * (0.045f + (i % 3) * 0.018f);
        fill_triangle(sx - w, (float)water, sx + w * 0.35f, (float)water,
                      sx + w * 2.4f, (float)ground, 0xb9f2ff, 0.030f);
    }

    draw_sand_bed(ground);

    fill_rect(0, water - 4.0f * G.scale, W, 5.0f * G.scale, 0xb9f2ff, 0.08f);
    draw_line(0, water, W, water, 2.0f * G.scale, 0xdff6ff, 0.24f);
    draw_line(0, G.groundY - 1.5f * G.scale, W, G.groundY - 1.5f * G.scale,
              2.0f * G.scale, 0xe7d69c, 0.38f);
    draw_backdrop_decor();
}

static void build_backdrop(void)
{
    if (!backdrop) return;
    uint32_t *old = canvas.px;
    canvas.px = backdrop;
    draw_background();
    canvas.px = old;
    backdropVersion = G.worldVersion;
}

static float water_surface_y(float x)
{
    if (W <= 1) return (float)G.waterY;
    float fx = clampf(x / (float)(W - 1), 0.0f, 1.0f) * (WATER_NODES - 1);
    int i = (int)floorf(fx);
    if (i < 0) i = 0;
    if (i >= WATER_NODES - 1) return G.waterY + G.waterLevel[WATER_NODES - 1];
    float t = fx - i;
    return G.waterY + G.waterLevel[i] * (1.0f - t) + G.waterLevel[i + 1] * t;
}

static float water_foam_at(float x)
{
    if (W <= 1) return 0.0f;
    float fx = clampf(x / (float)(W - 1), 0.0f, 1.0f) * (WATER_NODES - 1);
    int i = (int)floorf(fx);
    if (i < 0) i = 0;
    if (i >= WATER_NODES - 1) return G.waterFoam[WATER_NODES - 1];
    float t = fx - i;
    return G.waterFoam[i] * (1.0f - t) + G.waterFoam[i + 1] * t;
}

static float water_visual_y(float x)
{
    float s = G.scale;
    return water_surface_y(x) +
           (sinf(G.currentPhase * 2.7f + x * 0.018f) +
            sinf(G.currentPhase * 1.35f + x * 0.041f) * 0.42f +
            sinf(G.currentPhase * 4.9f + x * 0.071f) * 0.18f) * 1.85f * s;
}

static void draw_water_surface(void)
{
    float s = G.scale;
    float step = fmaxf(4.0f * s, W / (float)(WATER_NODES - 1));
    float lastX = 0.0f;
    float lastY = water_visual_y(0.0f);

    for (float x = 0.0f; x < W; x += fmaxf(2.0f, 3.0f * s)) {
        float y = water_visual_y(x);
        float foam = water_foam_at(x);
        fill_rect(x, y - 2.0f * s, fmaxf(2.0f, 3.0f * s), 9.0f * s,
                  0x8fe8ff, 0.055f + foam * 0.030f);
        fill_rect(x, y + 4.0f * s, fmaxf(2.0f, 3.0f * s), 18.0f * s,
                  0x053f62, 0.045f);
    }

    for (float x = step; x <= W + step * 0.5f; x += step) {
        float xx = fminf(x, (float)(W - 1));
        float y = water_visual_y(xx);
        float slope = fabsf(y - lastY) / fmaxf(1.0f, xx - lastX);
        float foam = fmaxf(water_foam_at(lastX), water_foam_at(xx));
        float crest = clampf(0.50f + slope * 0.52f + foam * 0.46f, 0.0f, 0.96f);
        draw_line(lastX, lastY + 5.0f * s, xx, y + 5.0f * s,
                  8.0f * s, 0x063a58, 0.24f);
        draw_line(lastX, lastY + 2.6f * s, xx, y + 2.6f * s,
                  3.0f * s, 0x0ea5c6, 0.32f + foam * 0.18f);
        draw_line(lastX, lastY - 0.4f * s, xx, y - 0.4f * s,
                  2.2f * s, 0xedfdff, crest);
        draw_line(lastX, lastY + 1.8f * s, xx, y + 1.8f * s,
                  1.2f * s, 0x67e8f9, 0.45f + foam * 0.22f);
        lastX = xx;
        lastY = y;
    }

    for (int i = 0; i < WATER_NODES; i += 4) {
        float amp = fabsf(G.waterLevel[i]);
        float foam = G.waterFoam[i];
        if (amp < 1.2f * s && foam < 0.04f) continue;
        float x = (float)i / (WATER_NODES - 1) * W;
        float y = water_visual_y(x);
        float a = clampf(amp / (18.0f * s) + foam * 0.58f, 0.0f, 0.72f);
        fill_ellipse(x, y - 1.5f * s,
                     fminf(7.0f * s, 2.0f * s + amp * 0.22f + foam * 4.0f * s),
                     fmaxf(0.7f * s, 1.3f * s), 0xffffff, a);
    }

    for (int band = 0; band < 3; band++) {
        float yOff = (15.0f + band * 13.0f) * s;
        float alpha = 0.050f - band * 0.010f;
        float lastSX = 0.0f;
        float lastSY = water_visual_y(0.0f) + yOff;
        for (float x = 20.0f * s; x < W; x += 36.0f * s) {
            float y = water_visual_y(x) + yOff +
                      sinf(G.currentPhase * (1.1f + band * 0.2f) + x * 0.024f + band) * 2.8f * s;
            draw_line(lastSX, lastSY, x, y, 1.0f * s, 0xb9f2ff, alpha);
            lastSX = x;
            lastSY = y;
        }
    }
}

static void draw_caustics(void)
{
    float s = G.scale;
    float t = G.frameCount / 60.0f;
    int bands = 8;
    for (int b = 0; b < bands; b++) {
        float yBase = G.waterY + (24.0f + b * 44.0f) * s;
        if (yBase > G.groundY - 40.0f * s) break;
        float lastX = 0.0f;
        float lastY = yBase + sinf(t * 0.7f + b) * 4.0f * s;
        for (float x = 28.0f * s; x < W; x += 38.0f * s) {
            float y = yBase + sinf(x * 0.012f + t * 1.4f + b * 0.8f) * 5.0f * s;
            draw_line(lastX, lastY, x, y, 1.0f * s, 0x9de9ff, 0.055f);
            lastX = x;
            lastY = y;
        }
    }

    float pulse = G.feedPulse > 0.0f ? clampf(G.feedPulse / 0.55f, 0.0f, 1.0f) : 0.0f;
    if (pulse > 0.0f) {
        ring(G.mouseX, G.mouseY, (18.0f + (1.0f - pulse) * 54.0f) * s,
             1.2f * s, 0xfef3c7, 0.35f * pulse);
    }
}

static void draw_rocks(void)
{
    float s = G.scale;
    for (int i = 0; i < G.numRocks; i++) {
        Rock *r = &G.rocks[i];
        const Sprite *sp = &rockSprites[(r->type % ENV_ROCK_ASSET_COUNT + ENV_ROCK_ASSET_COUNT) %
                                        ENV_ROCK_ASSET_COUNT];
        if (!sp->px || sp->w <= 0) continue;
        float sx = clampf(r->w / fmaxf(1.0f, (float)sp->w), 0.62f, 1.42f);
        float sy = clampf((r->h * 2.4f) / fmaxf(1.0f, (float)sp->h), 0.62f, 1.28f);
        float sc = (sx + sy) * 0.5f;
        float base = G.groundY + 5.0f * s;
        fill_ellipse(r->x, base - 3.0f * s, sp->w * sc * 0.40f,
                     fmaxf(2.8f * s, sp->h * sc * 0.075f), 0x020b0f, 0.28f);
        blit_sprite_scaled(sp, r->x, base, r->flip, 0.98f, sc, sc);
    }
}

static void draw_plants(void)
{
    float s = G.scale;
    for (int i = 0; i < MAX_PLANTS; i++) {
        Plant *p = &G.plants[i];
        if (!p->active) continue;
        const Sprite *sp = &plantSprites[(p->type % ENV_PLANT_ASSET_COUNT + ENV_PLANT_ASSET_COUNT) %
                                         ENV_PLANT_ASSET_COUNT];
        if (!sp->px || sp->h <= 0) continue;
        float bend = sinf(p->phase + G.frameCount * 0.018f * p->sway);
        float sy = clampf(p->height / fmaxf(1.0f, (float)sp->h), 0.46f, 1.72f);
        float sx = sy * p->scale * (0.94f + bend * 0.035f);
        float x = p->x + bend * p->sway * 4.6f * s;
        float base = G.groundY + 4.0f * s;
        fill_ellipse(x, base - 2.0f * s, sp->w * sx * 0.34f,
                     fmaxf(2.0f * s, sp->h * sy * 0.028f), 0x021016, 0.22f);
        blit_sprite_tint_scaled(sp, x + 1.8f * s, base + 2.2f * s, p->flip,
                                0x03141c, 0.24f, 1.0f * s, sx, sy);
        blit_sprite_scaled(sp, x, base, p->flip, 0.96f, sx, sy);
    }
}

static void draw_bubbles(void)
{
    for (int i = 0; i < MAX_BUBBLES; i++) {
        Bubble *b = &G.bubbles[i];
        if (!b->active) continue;
        int tier = (int)clampf((b->size / fmaxf(0.1f, G.scale)) - 1.2f, 0.0f, 4.0f);
        blit_sprite(&bubbleSprites[tier], b->x, b->y, false, 0.92f);
    }
}

static void draw_food(void)
{
    float s = G.scale;
    for (int i = 0; i < MAX_FOOD; i++) {
        Food *f = &G.food[i];
        if (!f->active) continue;
        fill_circle(f->x, f->y, 1.8f * s, 0xd19a58, 0.96f);
        fill_circle(f->x - 0.6f * s, f->y - 0.6f * s, 0.7f * s, 0xffe0a3, 0.80f);
    }
}

static void draw_fish_one(const Fish *f)
{
    float s = G.scale;
    float face = fabsf(f->facing) > 0.04f ? f->facing : (float)f->direction;
    float d = face >= 0.0f ? 1.0f : -1.0f;
    bool flip = face < 0.0f;
    float turn = 1.0f - clampf(fabsf(f->facing), 0.0f, 1.0f);
    float smoothTurn = turn * turn * (3.0f - 2.0f * turn);
    float sxScale = 1.0f - smoothTurn * 0.22f;
    float syScale = 1.0f + smoothTurn * 0.04f;
    int tier = fish_tier(f->size);
    int pal = f->palette % FISH_PALETTE_COUNT;
    int species = f->species;
    if (species < 0 || species >= FISH_SPECIES_COUNT) species = 0;
    if (!fish_asset_for_species(species).tint) pal = 0;
    bool attack = f->attackTimer > 0.0f;
    int frame = attack ? 4 + ((G.frameCount / 2) & 3)
                       : ((int)floorf(f->tailPhase * (2.0f / PI)) & 3);
    bool turnPose = turn > 0.04f || f->turnTimer > 0.0f;
    int turnFrame = (int)lroundf(clampf((1.0f - f->facing) * 0.5f, 0.0f, 1.0f) *
                                 (FISH_TURN_FRAMES - 1));
    if (turnFrame < 0) turnFrame = 0;
    if (turnFrame >= FISH_TURN_FRAMES) turnFrame = FISH_TURN_FRAMES - 1;
    const Sprite *body = &fishSprites[species][tier][pal][frame];
    const SpriteAtlas *turnAtlas = &fishTurnAtlases[species][tier][pal];
    const AtlasFrame *turnFr = atlas_frame(turnAtlas, turnFrame);
    float turnScale = turnFr ? body->w / fmaxf(1.0f, (float)turnFr->w) : 1.0f;
    float drawSx = turnPose ? turnScale : sxScale;
    float drawSy = turnPose ? turnScale : syScale;

    if (attack) {
        float a = clampf(f->attackTimer / 0.44f, 0.0f, 1.0f);
        uint32_t col = scale_rgb(FISH_PALETTES[pal].fin, 1.20f);
        float tailX = f->x - d * (18.0f + tier * 5.0f) * s;
        for (int i = 0; i < 3; i++) {
            float off = (i - 1) * 3.0f * s;
            draw_line(tailX - d * (18.0f + i * 8.0f) * s, f->y + off,
                      f->x - d * (6.0f + i * 2.0f) * s, f->y + off * 0.35f,
                      (1.0f + i * 0.35f) * s, col, 0.13f * a);
        }
        ring(f->attackX, f->attackY, (7.0f + (1.0f - a) * 14.0f) * s,
             0.9f * s, 0xfff1b8, 0.24f * a);
        if (turnPose)
            blit_atlas_tint_scaled(turnAtlas, turnFrame, f->x, f->y,
                                   false, 0xfff1a8, 0.18f * a, 2.0f,
                                   drawSx, drawSy);
        else
            blit_sprite_tint_scaled(body, f->x, f->y,
                                    flip, 0xfff1a8, 0.18f * a, 2.0f,
                                    drawSx, drawSy);
    }

    if (turnPose) {
        blit_atlas_tint_scaled(turnAtlas, turnFrame, f->x + 2.0f * s, f->y + 2.5f * s,
                               false, 0x00111a, 0.34f, 0.0f, drawSx, drawSy);
        blit_atlas_scaled(turnAtlas, turnFrame, f->x, f->y,
                          false, 1.0f, drawSx, drawSy);
    } else {
        blit_sprite_tint_scaled(body, f->x + 2.0f * s, f->y + 2.5f * s,
                                flip, 0x00111a, 0.34f, 0.0f, drawSx, drawSy);
        blit_sprite_scaled(body, f->x, f->y,
                           flip, 1.0f, drawSx, drawSy);
    }
    if (turn > 0.18f) {
        float a = clampf((turn - 0.18f) / 0.82f, 0.0f, 1.0f);
        draw_line(f->x, f->y - (8.0f + tier * 2.0f) * s,
                  f->x, f->y + (8.0f + tier * 2.0f) * s,
                  (1.2f + tier * 0.3f) * s, 0xe0faff, 0.34f * a);
        fill_ellipse(f->x - d * (2.0f + tier) * s, f->y - 1.0f * s,
                     (2.2f + tier) * s, (5.0f + tier * 1.5f) * s,
                     scale_rgb(FISH_PALETTES[pal].body, 1.35f), 0.16f * a);
    }
    if (G.showInfo && f->biteTimer > 0.0f) {
        char pts[32];
        float a = clampf(f->biteTimer / 0.48f, 0.0f, 1.0f);
        snprintf(pts, sizeof pts, G.combo > 1 ? "+%d x%d" : "+%d",
                 10 + G.combo * 2, G.combo);
        draw_text_shadow(f->attackX + 8.0f * s, f->attackY - 20.0f * s,
                         pts, 0xfff1a8, a, 1);
    }
}

static void draw_fish(void)
{
    for (int i = 0; i < MAX_FISH; i++)
        if (G.fish[i].active)
            draw_fish_one(&G.fish[i]);
}

static void shark_bite_mouth(const SharkBite *b, float *mx, float *my, int *dir)
{
    float s = G.scale;
    *mx = b->mouthX;
    *my = b->mouthY;
    *dir = b->direction >= 0 ? 1 : -1;
    if (b->sharkIndex >= 0 && b->sharkIndex < MAX_SHARKS && G.sharks[b->sharkIndex].active) {
        const Shark *sh = &G.sharks[b->sharkIndex];
        float face = fabsf(sh->facing) > 0.05f ? sh->facing : (float)sh->direction;
        *dir = face >= 0.0f ? 1 : -1;
        *mx = sh->x + *dir * 54.0f * s;
        *my = sh->y + 3.0f * s;
    }
}

static void draw_shark_bite_one(const SharkBite *b)
{
    if (!b->active || b->maxLife <= 0.0f) return;

    float s = G.scale;
    float t = clampf((b->maxLife - b->life) / b->maxLife, 0.0f, 1.0f);
    float mx, my;
    int dir;
    shark_bite_mouth(b, &mx, &my, &dir);

    float pull = clampf((t - 0.04f) / 0.48f, 0.0f, 1.0f);
    pull = pull * pull * (3.0f - 2.0f * pull);
    float crush = clampf((t - 0.18f) / 0.38f, 0.0f, 1.0f);
    float swallow = clampf((t - 0.54f) / 0.30f, 0.0f, 1.0f);
    float fade = clampf((0.94f - t) / 0.30f, 0.0f, 1.0f);
    float shake = sinf(t * PI * 22.0f) * (1.0f - swallow) * 2.2f * s;
    float px = b->x + (mx - dir * (9.0f + 6.0f * swallow) * s - b->x) * pull;
    float py = b->y + (my + sinf(t * PI * 5.0f) * 3.4f * s - b->y) * pull + shake;

    float burst = clampf(t / 0.18f, 0.0f, 1.0f) * clampf((0.92f - t) / 0.30f, 0.0f, 1.0f);
    fill_ellipse(mx - dir * (9.0f + 8.0f * t) * s, my + 2.0f * s,
                 (14.0f + 34.0f * t) * s, (7.0f + 14.0f * t) * s,
                 0x3b0308, 0.46f * burst);
    fill_ellipse(mx - dir * (3.0f + 5.0f * t) * s, my + 1.0f * s,
                 (9.0f + 18.0f * t) * s, (4.5f + 9.0f * t) * s,
                 0xdc2626, 0.36f * burst);
    ring(mx - dir * 3.0f * s, my, (8.0f + t * 22.0f) * s,
         (1.0f + t * 1.4f) * s, 0xb80f1f, 0.20f * burst);

    for (int i = 0; i < 26; i++) {
        uint32_t h = b->seed ^ (uint32_t)(i * 747796405u);
        float r0 = hash01(h);
        float r1 = hash01(h ^ 0x9e3779b9u);
        float r2 = hash01(h ^ 0x85ebca6bu);
        float r3 = hash01(h ^ 0xc2b2ae35u);
        float local = clampf((t - r0 * 0.52f) / 0.58f, 0.0f, 1.0f);
        if (local <= 0.0f || local >= 1.0f) continue;
        float side = (r1 - 0.5f) * (20.0f + 24.0f * local) * s;
        float back = (8.0f + r2 * 64.0f) * local * s;
        float dx = mx - dir * back + side * 0.25f;
        float dy = my + side * 0.42f + (r3 - 0.35f) * 22.0f * local * s;
        uint32_t col = (i & 3) == 0 ? 0xdc2626 : ((i & 1) ? 0x7f0612 : 0x4b0208);
        float a = (1.0f - local) * (0.26f + 0.62f * burst);
        float rr = (1.2f + r2 * 3.6f) * s * (1.0f - local * 0.35f);
        fill_circle(dx, dy, rr, col, a);
        if ((i % 5) == 0)
            draw_line(dx + dir * rr * 2.8f, dy - rr * 0.4f, dx, dy,
                      fmaxf(0.9f * s, rr * 0.70f), col, a * 0.55f);
    }

    int species = b->species;
    if (species < 0 || species >= FISH_SPECIES_COUNT) species = 0;
    int pal = b->palette % FISH_PALETTE_COUNT;
    if (!fish_asset_for_species(species).tint) pal = 0;
    int tier = fish_tier(b->victimSize);
    int phase = (int)clampf(floorf(t * 7.0f), 0.0f, 6.0f);
    int frame = phase < 4 ? 4 + phase : 4 + ((G.frameCount / 2) & 3);
    const Sprite *body = &fishSprites[species][tier][pal][frame % FISH_SWIM_FRAMES];
    bool flip = dir < 0;
    float biteScale = 1.0f - 0.56f * swallow;
    float sx = biteScale * (1.0f - 0.48f * crush);
    float sy = biteScale * (1.0f + 0.14f * crush - 0.58f * swallow);

    if (fade > 0.02f && body->px) {
        blit_sprite_tint_scaled(body, px + 2.0f * s, py + 2.5f * s, flip,
                                0x120105, 0.38f * fade, 0.0f, sx, sy);
        blit_sprite_tint_scaled(body, px, py, flip,
                                0xb80f1f, (0.10f + 0.26f * crush) * fade,
                                1.0f * s, sx, sy);
        blit_sprite_scaled(body, px, py, flip, fade, sx, sy);
        if (crush > 0.12f) {
            float woundX = px + dir * (body->w * sx * 0.24f);
            fill_circle(woundX, py + 1.5f * s, (3.8f + 4.0f * crush) * s,
                        0xb80f1f, 0.58f * crush * fade);
        }
    }

    if (t < 0.70f) {
        float snap = clampf((0.70f - t) / 0.70f, 0.0f, 1.0f);
        draw_line(mx - dir * 5.0f * s, my - 7.0f * s,
                  mx + dir * 14.0f * s, my - 1.5f * s,
                  1.3f * s, 0xfff7ed, 0.50f * snap);
        draw_line(mx - dir * 5.0f * s, my + 7.0f * s,
                  mx + dir * 14.0f * s, my + 1.5f * s,
                  1.3f * s, 0xfff7ed, 0.46f * snap);
    }

    if (t > 0.48f) {
        float a = clampf((t - 0.48f) / 0.22f, 0.0f, 1.0f) *
                  clampf((0.96f - t) / 0.28f, 0.0f, 1.0f);
        for (int i = 0; i < 4; i++) {
            float off = (i - 1.5f) * 4.2f * s;
            draw_line(mx - dir * (4.0f + i * 4.0f) * s, my + off,
                      mx - dir * (14.0f + i * 6.0f) * s, my + off * 0.35f,
                      1.0f * s, 0xfff7ed, 0.35f * a);
        }
    }
}

static void draw_shark_bites(void)
{
    for (int i = 0; i < MAX_SHARK_BITES; i++)
        if (G.sharkBites[i].active)
            draw_shark_bite_one(&G.sharkBites[i]);
}

static void draw_shark_one(const Shark *sh)
{
    float s = G.scale;
    float face = fabsf(sh->facing) > 0.04f ? sh->facing : (float)sh->direction;
    float d = face >= 0.0f ? 1.0f : -1.0f;
    bool flip = face < 0.0f;
    float turn = 1.0f - clampf(fabsf(sh->facing), 0.0f, 1.0f);
    float smoothTurn = turn * turn * (3.0f - 2.0f * turn);
    float sxScale = 1.0f - smoothTurn * 0.66f;
    float syScale = 1.0f + smoothTurn * 0.10f;
    bool attack = sh->attackTimer > 0.0f || sh->hunting;
    int frame = attack ? 3 + ((G.frameCount / 3) % 3)
                       : ((int)floorf(sh->tailPhase * (3.0f / (PI * 2.0f))) % 3);
    if (frame < 0) frame = 0;
    bool turnPose = turn > 0.04f || sh->turnTimer > 0.0f;
    int turnFrame = (int)lroundf(clampf((1.0f - sh->facing) * 0.5f, 0.0f, 1.0f) *
                                 (SHARK_TURN_FRAMES - 1));
    if (turnFrame < 0) turnFrame = 0;
    if (turnFrame >= SHARK_TURN_FRAMES) turnFrame = SHARK_TURN_FRAMES - 1;
    const AtlasFrame *turnFr = atlas_frame(&sharkTurnAtlas, turnFrame);
    float turnScale = turnFr ? sharkSprites[frame].w / fmaxf(1.0f, (float)turnFr->w) : 1.0f;
    float drawSx = turnPose ? turnScale : sxScale;
    float drawSy = turnPose ? turnScale : syScale;

    if (attack) {
        float a = sh->attackTimer > 0.0f ? clampf(sh->attackTimer / 0.58f, 0.18f, 1.0f) : 0.28f;
        for (int i = 0; i < 4; i++) {
            float len = (34.0f + i * 12.0f) * s;
            draw_line(sh->x - d * len, sh->y + (i - 1.5f) * 4.0f * s,
                      sh->x - d * (14.0f + i * 5.0f) * s, sh->y + (i - 1.5f) * 1.5f * s,
                      (1.1f + i * 0.25f) * s, 0xd7f5ff, 0.11f * a);
        }
        if (turnPose)
            blit_atlas_tint_scaled(&sharkTurnAtlas, turnFrame, sh->x, sh->y, false,
                                   0xff3b4f, 0.18f * a, 2.0f,
                                   drawSx, drawSy);
        else
            blit_sprite_tint_scaled(&sharkSprites[frame], sh->x, sh->y, flip,
                                    0xff3b4f, 0.18f * a, 2.0f,
                                    drawSx, drawSy);
    }

    if (turnPose) {
        blit_atlas_tint_scaled(&sharkTurnAtlas, turnFrame, sh->x + 3.0f * s, sh->y + 3.4f * s,
                               false, 0x00111a, 0.42f, 0.0f, drawSx, drawSy);
        blit_atlas_scaled(&sharkTurnAtlas, turnFrame, sh->x, sh->y, false, 1.0f,
                          drawSx, drawSy);
    } else {
        blit_sprite_tint_scaled(&sharkSprites[frame], sh->x + 3.0f * s, sh->y + 3.4f * s,
                                flip, 0x00111a, 0.42f, 0.0f, drawSx, drawSy);
        blit_sprite_scaled(&sharkSprites[frame], sh->x, sh->y, flip, 1.0f,
                           drawSx, drawSy);
    }
    if (turn > 0.15f) {
        float a = clampf((turn - 0.15f) / 0.85f, 0.0f, 1.0f);
        draw_line(sh->x, sh->y - 21.0f * s, sh->x, sh->y + 19.0f * s,
                  2.4f * s, 0xe0faff, 0.26f * a);
        fill_ellipse(sh->x - d * 4.0f * s, sh->y,
                     6.0f * s, 22.0f * s, 0xb9c7d4, 0.12f * a);
    }
}

static void draw_sharks(void)
{
    for (int i = 0; i < MAX_SHARKS; i++)
        if (G.sharks[i].active)
            draw_shark_one(&G.sharks[i]);
}

static void draw_cannonballs(void)
{
    float s = G.scale;
    for (int i = 0; i < MAX_CANNONBALLS; i++) {
        Cannonball *ball = &G.cannonballs[i];
        if (!ball->active) continue;
        float sp = sqrtf(ball->vx * ball->vx + ball->vy * ball->vy);
        float tx = sp > 1.0f ? ball->vx / sp : 0.0f;
        float ty = sp > 1.0f ? ball->vy / sp : 0.0f;
        draw_line(ball->x - tx * 18.0f * s, ball->y - ty * 18.0f * s,
                  ball->x, ball->y, 2.0f * s, 0x9fb3c8, 0.24f);
        fill_circle(ball->x + 1.2f * s, ball->y + 1.8f * s, 3.2f * s, 0x00111a, 0.34f);
        fill_circle(ball->x, ball->y, 3.0f * s, 0x1f2937, 1.0f);
        fill_circle(ball->x - 0.9f * s, ball->y - 1.0f * s, 1.0f * s, 0xdbeafe, 0.42f);
    }
}

static void draw_ship_one(const Ship *ship)
{
    float shipS = G.scale;
    float shipX = ship->x;
    float face = fabsf(ship->facing) > 0.05f ? ship->facing : (float)ship->direction;
    float d = face >= 0.0f ? 1.0f : -1.0f;
    bool flip = face < 0.0f;
    float surface = water_surface_y(shipX);
    float bob = sinf(ship->bobPhase) * 1.8f * shipS;
    float shipY = surface + bob;
    draw_line(shipX - 62.0f * shipS, surface + 3.2f * shipS,
              shipX + 62.0f * shipS, surface + 3.2f * shipS,
              3.0f * shipS, 0xdffbff, 0.28f);
    draw_line(shipX - 48.0f * shipS, surface + 6.2f * shipS,
              shipX + 52.0f * shipS, surface + 6.2f * shipS,
              2.0f * shipS, 0x0b6b8d, 0.28f);

    if (ship->fishing) {
        float rodX = shipX + d * 22.0f * shipS;
        float rodY = shipY - 24.0f * shipS;
        draw_line(rodX, rodY, ship->hookX, ship->hookY,
                  fmaxf(1.0f, 0.9f * shipS), 0xd6b26f, 0.82f);
        draw_line(ship->hookX, ship->hookY,
                  ship->hookX + d * 5.0f * shipS, ship->hookY + 4.0f * shipS,
                  fmaxf(1.0f, 1.4f * shipS), 0xe5e7eb, 0.92f);
        fill_circle(ship->hookX, ship->hookY, 2.2f * shipS, 0xf8fafc, 0.92f);
        if (ship->fishingPulse > 0.0f) {
            float a = clampf(ship->fishingPulse / 0.55f, 0.0f, 1.0f);
            ring(ship->hookX, ship->hookY, (8.0f + (1.0f - a) * 18.0f) * shipS,
                 1.0f * shipS, 0xfff1a8, 0.32f * a);
        }
        if (G.showInfo && ship->biteTimer > 0.0f) {
            float a = clampf(ship->biteTimer / 1.08f, 0.0f, 1.0f);
            ring(ship->hookX, ship->hookY, (11.0f + (1.0f - a) * 13.0f) * shipS,
                 1.2f * shipS, 0xffd166, 0.62f);
            draw_text_shadow(ship->hookX + 9.0f * shipS, ship->hookY - 23.0f * shipS,
                             "BITE", 0xfff1a8, 0.95f, 1);
        }
        if (ship->hookedFish >= 0) {
            float bx = clampf(ship->hookX - 42.0f * shipS, 8.0f * shipS, W - 96.0f * shipS);
            float by = clampf(ship->hookY + 18.0f * shipS, G.waterY + 12.0f * shipS,
                              G.groundY - 48.0f * shipS);
            uint32_t tensionCol = ship->tension < 0.30f ? 0x7dd3fc :
                                  ship->tension <= 0.78f ? 0x22c55e : 0xfb7185;
            fill_rect(bx, by, 86.0f * shipS, 22.0f * shipS, 0x03131d, 0.76f);
            stroke_rect(bx, by, 86.0f * shipS, 22.0f * shipS,
                        fmaxf(1.0f, 1.0f * shipS), 0x8fdaf0, 0.32f);
            fill_rect(bx + 5.0f * shipS, by + 5.0f * shipS,
                      76.0f * shipS * clampf(ship->tension, 0.0f, 1.0f),
                      5.0f * shipS, tensionCol, 0.90f);
            fill_rect(bx + 5.0f * shipS, by + 13.0f * shipS,
                      76.0f * shipS * clampf(ship->catchMeter, 0.0f, 1.0f),
                      4.0f * shipS, 0xffd166, 0.86f);
            fill_rect(bx + 5.0f * shipS + 76.0f * shipS * 0.30f, by + 4.0f * shipS,
                      76.0f * shipS * 0.48f, 7.0f * shipS, 0x22c55e, 0.18f);
        }
    }

    if (ship->cannonFlash > 0.0f) {
        float a = clampf(ship->cannonFlash / 0.22f, 0.0f, 1.0f);
        float mx = shipX + d * 50.0f * shipS;
        float my = shipY - 22.0f * shipS;
        fill_circle(mx, my, (5.5f + (1.0f - a) * 8.0f) * shipS, 0xfff1a8, 0.38f * a);
        fill_circle(mx + d * 4.0f * shipS, my, 3.4f * shipS, 0xff8a3d, 0.60f * a);
    }

    blit_sprite_tint(&boatSprite, shipX + 3.0f * shipS, shipY + 2.0f * shipS,
                     flip, 0x00111a, 0.30f, 0.0f);
    blit_sprite(&boatSprite, shipX, shipY, flip, 1.0f);

    if (game_count_ships() > 1) {
        float baseX = shipX - 18.0f * shipS;
        float baseY = surface + 11.0f * shipS;
        for (int h = 0; h < 3; h++) {
            uint32_t col = h < ship->health ? 0xffd166 : 0x334155;
            fill_rect(baseX + h * 13.0f * shipS, baseY, 8.0f * shipS, 3.0f * shipS,
                      col, h < ship->health ? 0.88f : 0.55f);
        }
    } else if (G.showInfo && ship->fishing) {
        draw_text_shadow(shipX - 34.0f * shipS, surface + 14.0f * shipS,
                         "FISHING", 0xfff1a8, 0.90f, 1);
    }
}

static void draw_ships(void)
{
    for (int i = 0; i < MAX_SHIPS; i++)
        if (G.ships[i].active)
            draw_ship_one(&G.ships[i]);
}

static void draw_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        float a = p->maxLife > 0.0f ? clampf(p->life / p->maxLife, 0.0f, 1.0f) : 1.0f;
        fill_circle(p->x, p->y, p->size * (0.4f + 0.6f * a), p->color, a);
    }
}

static void draw_cursor_marker(void)
{
    float s = G.scale;
    float x = clampf((float)G.mouseX, 0.0f, (float)(W - 1));
    float y = clampf((float)G.mouseY, 0.0f, (float)(H - 1));
    bool fishing = false;
    for (int i = 0; i < MAX_SHIPS; i++)
        if (G.ships[i].active && G.ships[i].fishing)
            fishing = true;

    uint32_t col = fishing ? 0xffd166 : 0xb9f2ff;
    float pulse = 0.75f + 0.25f * sinf(G.currentPhase * 5.0f);
    ring(x, y, (8.0f + pulse * 2.0f) * s, 1.2f * s, col, 0.78f);
    draw_line(x - 15.0f * s, y, x - 5.0f * s, y, 1.2f * s, col, 0.72f);
    draw_line(x + 5.0f * s, y, x + 15.0f * s, y, 1.2f * s, col, 0.72f);
    draw_line(x, y - 15.0f * s, x, y - 5.0f * s, 1.2f * s, col, 0.72f);
    draw_line(x, y + 5.0f * s, x, y + 15.0f * s, 1.2f * s, col, 0.72f);
    fill_circle(x, y, 1.8f * s, 0xffffff, 0.86f);
}

static void panel(float w, float h, float *px, float *py)
{
    float s = G.scale;
    *px = (W - w) * 0.5f;
    *py = (H - h) * 0.5f;
    fill_rect(*px - 8.0f * s, *py - 8.0f * s, w + 16.0f * s, h + 16.0f * s,
              0x04131e, 0.60f);
    fill_rect(*px, *py, w, h, 0x082033, 0.94f);
    stroke_rect(*px, *py, w, h, fmaxf(1.0f, 1.6f * s), 0x8fdaf0, 0.42f);
}

static void hud_chip(float x, float y, const char *label, uint32_t col)
{
    float s = G.scale;
    float w = text_width(label, 1) + 18.0f * s;
    float h = 18.0f * s;
    fill_rect(x, y, w, h, 0x041827, 0.78f);
    stroke_rect(x, y, w, h, fmaxf(1.0f, s), col, 0.42f);
    fill_rect(x + 2.0f * s, y + 2.0f * s, 4.0f * s, h - 4.0f * s, col, 0.68f);
    draw_text_shadow(x + 10.0f * s, y + 4.0f * s, label, 0xdffbff, 1.0f, 1);
}

static void hud_meter(float x, float y, float w, float h, float value,
                      uint32_t fill, const char *label)
{
    value = clampf(value, 0.0f, 1.0f);
    fill_rect(x, y, w, h, 0x03131d, 0.92f);
    stroke_rect(x, y, w, h, fmaxf(1.0f, G.scale), 0x7dd3fc, 0.28f);
    fill_rect(x + 2.0f, y + 2.0f, (w - 4.0f) * value, h - 4.0f, fill, 0.88f);
    fill_rect(x + 2.0f, y + 2.0f, (w - 4.0f) * value, fmaxf(1.0f, h * 0.32f),
              0xffffff, 0.16f);
    draw_text_shadow(x + 6.0f * G.scale, y + h * 0.5f - 7.0f * G.scale,
                     label, 0xe0faff, 0.92f, 1);
}

static void draw_hud(void)
{
    float s = G.scale;
    if (G.showInfo) {
        char buf[160];
        float topH = 48.0f * s;
        uint32_t topCol = G.dangerPulse > 0.0f ? 0x2b0810 : 0x03131d;
        fill_rect(0, 0, W, topH, topCol, 0.84f);
        fill_rect(0, topH - 3.0f * s, W, 3.0f * s,
                  G.frenzyTimer > 0.0f ? 0xfff1a8 : 0x38bdf8, 0.75f);
        if (G.scorePulse > 0.0f)
            fill_rect(0, 0, W, topH, 0xfff1a8, 0.10f * clampf(G.scorePulse / 0.38f, 0.0f, 1.0f));
        if (G.dangerPulse > 0.0f)
            fill_rect(0, 0, W, topH, 0xff1f3d, 0.16f * clampf(G.dangerPulse / 0.72f, 0.0f, 1.0f));

        draw_text_shadow(13.0f * s, 8.0f * s, "REEF RUSH", 0xb9f2ff, 1.0f, 2);
        snprintf(buf, sizeof buf, "SCORE %06d", G.score);
        draw_text_shadow(190.0f * s, 10.0f * s, buf, 0xfff1a8, 1.0f, 1);
        snprintf(buf, sizeof buf, "LV %02d", G.level);
        draw_text_shadow(322.0f * s, 10.0f * s, buf, 0xa7f3d0, 1.0f, 1);
        snprintf(buf, sizeof buf, "FISH %02d  SHARK %d  BOAT %d  FOOD %03d",
                 game_count_fish(), game_count_sharks(), game_count_ships(), game_count_food());
        draw_text_shadow(392.0f * s, 10.0f * s, buf, 0xe0faff, 1.0f, 1);

        if (G.combo > 1) {
            snprintf(buf, sizeof buf, "COMBO x%d", G.combo);
            draw_text_shadow(W - 292.0f * s, 10.0f * s, buf, 0xf9a8d4, 1.0f, 1);
        }
        const char *meterLabel = G.frenzyTimer > 0.0f ? "FRENZY" : "FEVER";
        hud_meter(W - 176.0f * s, 8.0f * s, 150.0f * s, 17.0f * s,
                  G.frenzyTimer > 0.0f ? G.frenzyTimer / 7.5f : G.frenzyMeter / 100.0f,
                  G.frenzyTimer > 0.0f ? 0xffd166 : 0x22d3ee, meterLabel);
        if (G.paused)
            draw_text_shadow(W - 96.0f * s, 29.0f * s, "PAUSED", 0xfef08a, 1.0f, 1);
    }

    if (G.showHelp) {
        fill_rect(0, H - 32.0f * s, W, 32.0f * s, 0x03131d, 0.76f);
        fill_rect(0, H - 32.0f * s, W, 2.0f * s, 0x38bdf8, 0.42f);
        if (W < 900) {
            hud_chip(12.0f * s, H - 25.0f * s, "CLICK FEED", 0xfacc15);
            hud_chip(126.0f * s, H - 25.0f * s, "F FISH", 0x22c55e);
            hud_chip(210.0f * s, H - 25.0f * s, "B BOAT", 0x7dd3fc);
            hud_chip(306.0f * s, H - 25.0f * s, "M FISH", 0xfff1a8);
            hud_chip(400.0f * s, H - 25.0f * s, "H HELP", 0x7dd3fc);
            hud_chip(492.0f * s, H - 25.0f * s, "Q QUIT", 0x94a3b8);
        } else {
            hud_chip(12.0f * s, H - 25.0f * s, "CLICK FEED", 0xfacc15);
            hud_chip(124.0f * s, H - 25.0f * s, "SPACE SPRINKLE", 0xf59e0b);
            hud_chip(266.0f * s, H - 25.0f * s, "F FISH", 0x22c55e);
            hud_chip(344.0f * s, H - 25.0f * s, "S SHARK", 0xfb7185);
            hud_chip(438.0f * s, H - 25.0f * s, "B BOAT", 0x7dd3fc);
            hud_chip(524.0f * s, H - 25.0f * s, "M FISH", 0xfff1a8);
            hud_chip(628.0f * s, H - 25.0f * s, "C CLEAR", 0xc084fc);
            hud_chip(718.0f * s, H - 25.0f * s, "P PAUSE", 0xfef08a);
            hud_chip(812.0f * s, H - 25.0f * s, "H HELP", 0x93c5fd);
            hud_chip(902.0f * s, H - 25.0f * s, "Q QUIT", 0x94a3b8);
        }
    }
}

static void draw_help(void)
{
    float s = G.scale;
    float pw = fminf(W - 72.0f * s, 640.0f * s);
    float ph = 332.0f * s;
    float px, py;
    panel(pw, ph, &px, &py);
    draw_text_center(W * 0.5f, py + 26.0f * s, "FISHTANK HELP", 0xb9f2ff, 1.0f, 2);
    draw_text(px + 52.0f * s, py + 76.0f * s,  "MOUSE             aim cursor; click feeds or strikes", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 104.0f * s, "SPACE / ENTER     sprinkle; in fishing, strike/reel", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 132.0f * s, "F / S / B         add fish, shark, boat", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 160.0f * s, "M                 fishing mode if one boat remains", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 188.0f * s, "FISHING           strike on BITE, reel in green tension", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 216.0f * s, "C / R / P / I     clear sharks, regenerate, pause, info", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 244.0f * s, "A                 toggle quiet water ambience", 0xf8fafc, 1.0f, 1);
    draw_text(px + 52.0f * s, py + 272.0f * s, "ESC / Q           close help, quit", 0xf8fafc, 1.0f, 1);
    draw_text_center(W * 0.5f, py + 308.0f * s, "MULTIPLE BOATS FIRE UNTIL ONE IS LEFT", 0x93c5fd, 1.0f, 1);
}

static void draw_pause(void)
{
    if (!G.paused || G.showHelp) return;
    float s = G.scale;
    float px, py;
    panel(330.0f * s, 132.0f * s, &px, &py);
    draw_text_center(W * 0.5f, py + 28.0f * s, "PAUSED", 0xfef08a, 1.0f, 2);
    draw_text_center(W * 0.5f, py + 84.0f * s, "P resume    H help    Q quit", 0xdbeafe, 1.0f, 1);
}

static void repack_frame(void)
{
    for (size_t i = 0, count = (size_t)W * H; i < count; i++) {
        uint32_t pixel = canvas.px[i];
        fb[i * 4 + 0] = (uint8_t)(pixel >> 16);
        fb[i * 4 + 1] = (uint8_t)(pixel >> 8);
        fb[i * 4 + 2] = (uint8_t)pixel;
        fb[i * 4 + 3] = (uint8_t)(pixel >> 24);
    }
}

void render_frame(void)
{
    if (!canvas.px || !fb) return;
    if (!backdrop || backdropVersion != G.worldVersion)
        build_backdrop();
    if (backdrop)
        memcpy(canvas.px, backdrop, (size_t)W * H * sizeof *backdrop);
    else
        draw_background();
    draw_caustics();
    draw_water_surface();
    draw_rocks();
    draw_plants();
    draw_bubbles();
    draw_food();
    draw_ships();
    draw_cannonballs();
    draw_fish();
    draw_sharks();
    draw_shark_bites();
    draw_particles();
    draw_cursor_marker();
    draw_hud();
    draw_pause();
    if (G.showHelp)
        draw_help();
    repack_frame();
}
