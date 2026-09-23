/* Software renderer: draws an RGBA framebuffer for the kitty presenter.
 *
 * All generic rasterization (blending, primitives, the 8x16 font) lives in
 * the vendored soft-raster library, which keeps the game's original
 * fixed-point coverage math byte for byte.  This file owns the
 * game-specific drawing: backdrop cache, starfield, bricks, paddle, balls,
 * HUD and overlays.  soft-raster canvases hold 0xAARRGGBB words; the
 * presenter and the PPM dumps consume R,G,B,A byte quadruplets, so
 * render_frame() finishes by repacking the canvas into a byte buffer.
 */
#include "kitty_brokeout.h"
#include "soft_raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f

static sr_canvas canvas;    /* frame being drawn, 0xAARRGGBB */
static sr_canvas backdrop;  /* cached gradient + vignette, 0xAARRGGBB */
static uint8_t *fb = NULL;  /* finished frame as R,G,B,A bytes */
static int W = 0, H = 0;
static int OX = 0, OY = 0;  /* camera shake offset, whole pixels */

uint8_t *render_fb(void) { return fb; }

/* Thin shims: same names and argument order the drawing code always used,
 * routed through soft-raster with the shake offset applied.  The offset is
 * integral, so shifting the float coordinates up front blends exactly the
 * same pixels the old per-pixel offset did. */
static inline void set_px(int x, int y, uint32_t rgb)
{
    sr_px(&canvas, x + OX, y + OY, rgb);
}

static void fill_rect(float x, float y, float w, float h, uint32_t rgb, float a)
{
    sr_fill_rect(&canvas, x + OX, y + OY, w, h, rgb, a);
}

static void stroke_rect(float x, float y, float w, float h,
                        float line, uint32_t rgb, float a)
{
    sr_stroke_rect(&canvas, x + OX, y + OY, w, h, line, rgb, a);
}

static void fill_circle(float cx, float cy, float r, uint32_t rgb, float a)
{
    sr_fill_circle(&canvas, cx + OX, cy + OY, r, rgb, a);
}

static void ring(float cx, float cy, float r, float width, uint32_t rgb, float a)
{
    sr_ring(&canvas, cx + OX, cy + OY, r, width, rgb, a);
}

static void draw_line(float x0, float y0, float x1, float y1,
                      float width, uint32_t rgb, float a)
{
    sr_line(&canvas, x0 + OX, y0 + OY, x1 + OX, y1 + OY, width, rgb, a, 0, 0);
}

static void draw_text(float fx, float fy, const char *s, uint32_t rgb, float a, int scale)
{
    sr_text(&canvas, fx + OX, fy + OY, s, rgb, a, scale);
}

static void draw_text_shadow(float x, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{
    sr_text_shadow(&canvas, x + OX, y + OY, s, rgb, a, scale);
}

static void draw_text_center(float cx, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{
    sr_text_center(&canvas, cx + OX, y + OY, s, rgb, a, scale);
}

void render_init(int w, int h)
{
    W = w;
    H = h;
    sr_canvas_init(&canvas, W, H);
    sr_canvas_init(&backdrop, W, H);
    fb = malloc((size_t)W * H * 4);

    for (int y = 0; y < H; y++) {
        float v = (float)y / fmaxf(1.0f, H - 1.0f);
        uint32_t top = 0x050713;
        uint32_t mid = 0x10142a;
        uint32_t bot = 0x13091b;
        uint32_t c = v < 0.58f ? sr_mix(top, mid, v / 0.58f)
                               : sr_mix(mid, bot, (v - 0.58f) / 0.42f);
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        for (int x = 0; x < W; x++) {
            float u = fabsf((float)x / fmaxf(1.0f, W - 1.0f) - 0.5f) * 2.0f;
            float vignette = 1.0f - 0.42f * powf(clampf(u * 0.9f + v * 0.35f, 0, 1), 1.6f);
            backdrop.px[(size_t)y * W + x] = 0xff000000u |
                ((uint32_t)(uint8_t)(r * vignette) << 16) |
                ((uint32_t)(uint8_t)(g * vignette) << 8) |
                (uint32_t)(uint8_t)(b * vignette);
        }
    }
}

void render_shutdown(void)
{
    sr_canvas_free(&canvas);
    sr_canvas_free(&backdrop);
    free(fb);
    fb = NULL;
}

static uint32_t brick_color(const Brick *b)
{
    if (b->type == BRICK_METAL) return 0xaab2c2;
    if (b->type == BRICK_EXPLOSIVE) return 0xd946ef;
    if (b->type == BRICK_SPEED) return 0x22d3ee;
    if (b->hits >= 3) return 0xef4444;
    if (b->hits == 2) return 0xf97316;
    return 0xfacc15;
}

static uint32_t powerup_color(int type)
{
    static const uint32_t colors[PU_COUNT] = {
        0x86efac, 0x7dd3fc, 0xf9a8d4, 0xc4b5fd
    };
    if (type < 0 || type >= PU_COUNT) return 0xffffff;
    return colors[type];
}

static bool render_has_attached_ball(void)
{
    for (int i = 0; i < MAX_BALLS; i++)
        if (G.balls[i].active && G.balls[i].attached)
            return true;
    return false;
}

static void draw_stars(void)
{
    float t = G.frameCount / 60.0f;
    for (int i = 0; i < G.numStars; i++) {
        Star *s = &G.stars[i];
        int b = (int)(s->brightness + sinf(t * s->speed + s->phase) * 34.0f);
        b = (int)clampf((float)b, 30, 255);
        uint32_t col = ((uint32_t)b << 16) | ((uint32_t)b << 8) | (uint32_t)clampf(b + 18, 0, 255);
        set_px((int)s->x, (int)s->y, col);
        if (s->size > 1 && b > 150) {
            int d = (int)(b * 0.42f);
            uint32_t dim = ((uint32_t)d << 16) | ((uint32_t)d << 8) | (uint32_t)d;
            set_px((int)s->x + 1, (int)s->y, dim);
            set_px((int)s->x, (int)s->y + 1, dim);
        }
    }
}

static void draw_grid(void)
{
    float s = G.scale;
    float step = 32.0f * s;
    float horizon = G.playY + G.playH + 4.0f * s;
    for (float x = G.playX; x <= G.playX + G.playW + 1; x += step) {
        float k = (x - G.playX) / G.playW;
        uint32_t col = sr_mix(0x172554, 0x581c87, k);
        draw_line(x, horizon, G.playX + G.playW * 0.5f + (x - (G.playX + G.playW * 0.5f)) * 0.42f,
                  G.playY + 80.0f * s, 1.0f, col, 0.12f);
    }
    for (int i = 0; i < 10; i++) {
        float y = horizon - i * 22.0f * s;
        if (y < G.playY + 68.0f * s) break;
        draw_line(G.playX, y, G.playX + G.playW, y, 1.0f, 0x1e3a8a, 0.10f);
    }
}

static void draw_playfield_frame(void)
{
    float s = G.scale;
    float x = G.playX, y = G.playY, w = G.playW, h = G.playH;
    fill_rect(x - 8 * s, y - 8 * s, w + 16 * s, h + 16 * s, 0x050510, 0.52f);
    stroke_rect(x - 2 * s, y - 2 * s, w + 4 * s, h + 4 * s, 2 * s, 0x38bdf8, 0.38f);
    stroke_rect(x - 6 * s, y - 6 * s, w + 12 * s, h + 12 * s, 1 * s, 0xa855f7, 0.20f);
    fill_rect(x, y, w, h, 0x020617, 0.23f);

    if (G.paddle.shieldTimer > 0.0f) {
        float a = clampf(G.paddle.shieldTimer / 12.0f, 0.18f, 1.0f);
        float sy = G.playY + G.playH - 6.0f * s;
        draw_line(x + 16 * s, sy, x + w - 16 * s, sy, 4.0f * s, 0x8b5cf6, 0.35f * a);
        draw_line(x + 16 * s, sy, x + w - 16 * s, sy, 1.2f * s, 0xf5d0fe, 0.92f * a);
    }
}

static void draw_brick(const Brick *b)
{
    if (!b->alive) return;
    float pulse = b->pulse;
    if (b->type == BRICK_EXPLOSIVE)
        pulse = fmaxf(pulse, 0.22f + 0.18f * sinf(G.frameCount * 0.13f + b->x * 0.01f));
    if (b->type == BRICK_SPEED)
        pulse = fmaxf(pulse, 0.13f + 0.12f * sinf(G.frameCount * 0.18f + b->y * 0.02f));

    uint32_t base = brick_color(b);
    float x = b->x, y = b->y, w = b->w, h = b->h;
    fill_rect(x - 3 * G.scale, y - 3 * G.scale, w + 6 * G.scale, h + 6 * G.scale,
              base, 0.10f + pulse * 0.16f);
    fill_rect(x, y, w, h, sr_scale_rgb(base, 0.58f), 1.0f);
    fill_rect(x + 1, y + 1, w - 2, h * 0.47f, sr_scale_rgb(base, 1.22f), 0.94f);
    fill_rect(x + 1, y + h * 0.52f, w - 2, h * 0.43f, sr_scale_rgb(base, 0.78f), 0.96f);
    stroke_rect(x, y, w, h, fmaxf(1.0f, G.scale), 0xffffff, 0.16f);
    fill_rect(x + 2 * G.scale, y + 2 * G.scale, w - 4 * G.scale, fmaxf(1, h * 0.12f),
              0xffffff, 0.18f);

    if (b->type == BRICK_METAL) {
        for (float sx = x - h; sx < x + w; sx += 11 * G.scale)
            draw_line(sx, y + h, sx + h, y, 1.0f * G.scale, 0xe5e7eb, 0.16f);
        fill_rect(x + w * 0.10f, y + h * 0.35f, w * 0.80f, h * 0.22f, 0x111827, 0.18f);
    } else if (b->type == BRICK_EXPLOSIVE) {
        fill_circle(x + w * 0.5f, y + h * 0.5f, h * 0.24f, 0xfff7ed, 0.72f);
        ring(x + w * 0.5f, y + h * 0.5f, h * 0.38f + pulse * h * 0.2f,
             1.0f * G.scale, 0xf0abfc, 0.55f);
    } else if (b->type == BRICK_SPEED) {
        float cy = y + h * 0.5f;
        for (int i = 0; i < 2; i++) {
            float ox = x + w * (0.36f + i * 0.18f);
            draw_line(ox - 4 * G.scale, cy - 4 * G.scale, ox + 2 * G.scale, cy,
                      2.0f * G.scale, 0xecfeff, 0.62f);
            draw_line(ox + 2 * G.scale, cy, ox - 4 * G.scale, cy + 4 * G.scale,
                      2.0f * G.scale, 0xecfeff, 0.62f);
        }
    } else if (b->hits < b->maxHits) {
        float cx = x + w * 0.52f;
        draw_line(cx - w * 0.26f, y + h * 0.22f, cx + w * 0.08f, y + h * 0.70f,
                  1.1f * G.scale, 0x451a03, 0.44f);
        draw_line(cx + w * 0.02f, y + h * 0.48f, cx + w * 0.30f, y + h * 0.32f,
                  1.0f * G.scale, 0x451a03, 0.35f);
    }
}

static void draw_bricks(void)
{
    for (int i = 0; i < G.numBricks; i++)
        draw_brick(&G.bricks[i]);
}

static void draw_paddle(void)
{
    Paddle *p = &G.paddle;
    float s = G.scale;
    uint32_t core = G.paddle.wideTimer > 0.0f ? 0x7dd3fc : 0x86efac;
    fill_rect(p->x - 12 * s, p->y - 9 * s, p->w + 24 * s, p->h + 22 * s,
              core, 0.12f);
    fill_rect(p->x, p->y, p->w, p->h, 0x052e16, 1.0f);
    fill_rect(p->x + 2 * s, p->y + 1 * s, p->w - 4 * s, p->h * 0.45f,
              sr_scale_rgb(core, 1.15f), 0.98f);
    fill_rect(p->x + 2 * s, p->y + p->h * 0.48f, p->w - 4 * s, p->h * 0.40f,
              sr_scale_rgb(core, 0.68f), 0.98f);
    stroke_rect(p->x, p->y, p->w, p->h, fmaxf(1.0f, s), 0xffffff, 0.28f);

    int segs = 7;
    for (int i = 1; i < segs; i++) {
        float x = p->x + p->w * i / segs;
        fill_rect(x, p->y + 2 * s, 1.0f * s, p->h - 4 * s, 0x022c22, 0.35f);
    }
    fill_circle(p->x + p->w * 0.5f, p->y + p->h * 0.5f, p->h * 0.35f,
                0xf0fdf4, 0.50f);
}

static void draw_ball(const Ball *b)
{
    if (!b->active) return;
    float s = G.scale;
    if (b->attached) {
        float len = 108.0f * s;
        float ex = b->x + sinf(G.launchAngle) * len;
        float ey = b->y - cosf(G.launchAngle) * len;
        draw_line(b->x, b->y, ex, ey, 2.0f * s, 0xfacc15, 0.34f);
        draw_line(b->x, b->y, ex, ey, 1.0f * s, 0xfffbeb, 0.52f);
        fill_circle(ex, ey, 3.2f * s, 0xfacc15, 0.58f);
    }
    for (int i = 9; i >= 1; i--) {
        int idx = (b->trailHead + 10 - i) % 10;
        float a = (10 - i) / 10.0f;
        fill_circle(b->trailX[idx], b->trailY[idx], b->radius * (0.45f + a * 0.55f),
                    0x93c5fd, 0.035f + a * 0.06f);
    }
    fill_circle(b->x, b->y, b->radius * 3.4f, 0x38bdf8, 0.08f);
    fill_circle(b->x, b->y, b->radius * 1.45f, 0x7dd3fc, 0.28f);
    fill_circle(b->x, b->y, b->radius, 0xf8fafc, 1.0f);
    fill_circle(b->x - b->radius * 0.28f, b->y - b->radius * 0.32f,
                fmaxf(1.0f, b->radius * 0.32f), 0xffffff, 0.9f);
    if (b->attached) {
        ring(b->x, b->y, b->radius + 7 * s + sinf(G.frameCount * 0.12f) * 2 * s,
             1.0f * s, 0xfacc15, 0.48f);
    }
}

static void draw_balls(void)
{
    for (int i = 0; i < MAX_BALLS; i++)
        draw_ball(&G.balls[i]);
}

static void draw_powerups(void)
{
    for (int i = 0; i < MAX_POWERUPS; i++) {
        Powerup *u = &G.powerups[i];
        if (!u->active) continue;
        float s = G.scale;
        float w = 39.0f * s;
        float h = 16.0f * s;
        float x = u->x - w * 0.5f;
        float y = u->y - h * 0.5f;
        uint32_t col = powerup_color(u->type);
        float bob = sinf(u->phase) * 1.5f * s;
        fill_rect(x - 4 * s, y + bob - 4 * s, w + 8 * s, h + 8 * s, col, 0.12f);
        fill_rect(x, y + bob, w, h, sr_scale_rgb(col, 0.72f), 1.0f);
        fill_rect(x + 2 * s, y + bob + 1 * s, w - 4 * s, h * 0.43f,
                  sr_scale_rgb(col, 1.20f), 0.95f);
        stroke_rect(x, y + bob, w, h, 1.0f * s, 0xffffff, 0.28f);
        char label[2] = { POWERUP_NAMES[u->type][0], 0 };
        draw_text_center(u->x, y + bob + h * 0.5f - 7 * s, label, 0x030712, 0.9f, 1);
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        float a = p->maxLife > 0 ? clampf(p->life / p->maxLife, 0, 1) : 1;
        float size = fmaxf(1.0f, p->size * (0.35f + 0.65f * a));
        if (size < 2.2f) fill_rect(p->x, p->y, size, size, p->color, a);
        else fill_circle(p->x, p->y, size * 0.5f, p->color, a);
    }
}

static void draw_hud(void)
{
    float s = G.scale;
    char buf[160];
    fill_rect(0, 0, W, 34 * s, 0x020617, 0.82f);
    draw_text_shadow(16 * s, 10 * s, "KILIX BROKEOUT", 0x7dd3fc, 1.0f, 1);

    snprintf(buf, sizeof buf, "SCORE %06d", G.score);
    draw_text_shadow(188 * s, 10 * s, buf, 0xf8fafc, 1.0f, 1);
    snprintf(buf, sizeof buf, "LEVEL %02d", G.level);
    draw_text_shadow(330 * s, 10 * s, buf, 0xfacc15, 1.0f, 1);
    snprintf(buf, sizeof buf, "LIVES %d", G.lives);
    draw_text_shadow(434 * s, 10 * s, buf, G.lives <= 1 ? 0xf87171 : 0x86efac, 1.0f, 1);
    snprintf(buf, sizeof buf, "BEST %06d", G.highScore);
    draw_text_shadow(520 * s, 10 * s, buf, 0xa5b4fc, 1.0f, 1);

    if (G.combo > 2 && G.comboTimer > 0.0f) {
        snprintf(buf, sizeof buf, "COMBO x%d", G.combo);
        draw_text_shadow(W - 250 * s, 10 * s, buf, 0xf9a8d4, 1.0f, 1);
    } else if (render_has_attached_ball()) {
        snprintf(buf, sizeof buf, "AIM %+03d", (int)(G.launchAngle * 180.0f / PI));
        draw_text_shadow(W - 250 * s, 10 * s, buf, 0xfacc15, 1.0f, 1);
    }
    if (G.speedBoostTimer > 0.0f)
        draw_text_shadow(W - 132 * s, 10 * s, "OVERDRIVE", 0x22d3ee, 1.0f, 1);

    fill_rect(0, H - 26 * s, W, 26 * s, 0x020617, 0.70f);
    draw_text(14 * s, H - 20 * s,
              W < 850 ? "LEFT/RIGHT move+aim   DOWN center   SPACE launch   P pause   Q quit"
                      : "LEFT/A RIGHT/D move + aim before launch   DOWN/S center aim   SPACE launch   P pause   Q quit",
              0x64748b, 1.0f, 1);
}

static void panel(float w, float h, float *px, float *py)
{
    *px = (W - w) * 0.5f;
    *py = (H - h) * 0.5f;
    fill_rect(*px - 10 * G.scale, *py - 10 * G.scale, w + 20 * G.scale, h + 20 * G.scale,
              0x38bdf8, 0.08f);
    fill_rect(*px - 3 * G.scale, *py - 3 * G.scale, w + 6 * G.scale, h + 6 * G.scale,
              0xa855f7, 0.24f);
    fill_rect(*px, *py, w, h, 0x09090f, 0.94f);
    stroke_rect(*px, *py, w, h, 2.0f * G.scale, 0x38bdf8, 0.42f);
}

static void draw_title(void)
{
    float s = G.scale;
    float pw = fminf(W - 70 * s, 720 * s);
    float ph = 320 * s;
    float px, py;
    panel(pw, ph, &px, &py);

    int titleScale = W >= 900 ? 3 : 2;
    draw_text_center(W * 0.5f, py + 36 * s, "KILIX BROKEOUT", 0x7dd3fc, 0.22f, titleScale);
    draw_text_center(W * 0.5f - 2, py + 34 * s - 2, "KILIX BROKEOUT", 0xf8fafc, 1.0f, titleScale);
    draw_text_center(W * 0.5f, py + 92 * s,
                     "native C brick breaker for kitty graphics terminals",
                     0xa5b4fc, 1.0f, 1);
    if (G.highScore > 0) {
        char score[64];
        snprintf(score, sizeof score, "BEST %06d", G.highScore);
        draw_text_center(W * 0.5f, py + 112 * s, score, 0xfacc15, 1.0f, 1);
    }

    float bx = px + 95 * s;
    float by = py + 128 * s;
    float bw = (pw - 190 * s) / 8.0f;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 8; c++) {
            uint32_t col = r == 0 ? 0xef4444 : r == 1 ? 0xf97316 : 0x22d3ee;
            fill_rect(bx + c * bw + 2 * s, by + r * 21 * s, bw - 4 * s, 14 * s,
                      sr_scale_rgb(col, 0.68f), 1.0f);
            fill_rect(bx + c * bw + 4 * s, by + r * 21 * s + 2 * s, bw - 8 * s, 5 * s,
                      sr_scale_rgb(col, 1.25f), 0.9f);
        }
    }
    fill_circle(W * 0.5f + 72 * s, py + 230 * s, 8 * s, 0xf8fafc, 1.0f);
    draw_line(W * 0.5f + 64 * s, py + 222 * s, W * 0.5f + 8 * s, py + 182 * s,
              2 * s, 0x93c5fd, 0.45f);
    fill_rect(W * 0.5f - 90 * s, py + 254 * s, 180 * s, 12 * s, 0x86efac, 0.9f);

    draw_text_center(W * 0.5f, py + 274 * s, "ENTER / SPACE  START", 0xfacc15, 1.0f, 1);
    draw_text_center(W * 0.5f, py + 298 * s, "C controls     M sound     Q quit", 0x94a3b8, 1.0f, 1);
}

static void draw_controls(void)
{
    float s = G.scale;
    float pw = fminf(W - 68 * s, 760 * s);
    float ph = 378 * s;
    float px, py;
    panel(pw, ph, &px, &py);
    draw_text_center(W * 0.5f, py + 28 * s, "CONTROLS", 0x7dd3fc, 1.0f, 2);
    draw_text(px + 74 * s, py + 82 * s,  "LEFT / A       move paddle left; aim left before launch", 0xf8fafc, 1.0f, 1);
    draw_text(px + 74 * s, py + 114 * s, "RIGHT / D      move paddle right; aim right before launch", 0xf8fafc, 1.0f, 1);
    draw_text(px + 74 * s, py + 146 * s, "DOWN / S       center launch aim", 0xf8fafc, 1.0f, 1);
    draw_text(px + 74 * s, py + 178 * s, "SPACE / ENTER  launch and advance", 0xf8fafc, 1.0f, 1);
    draw_text(px + 74 * s, py + 210 * s, "P              pause", 0xf8fafc, 1.0f, 1);
    draw_text(px + 74 * s, py + 258 * s, "Metal bricks bounce. Purple bricks explode.", 0xa5b4fc, 1.0f, 1);
    draw_text(px + 74 * s, py + 286 * s, "Cyan bricks trigger overdrive but drop slow capsules.", 0xa5b4fc, 1.0f, 1);
    draw_text_center(W * 0.5f, py + 344 * s, "ENTER / ESC  BACK", 0x64748b, 1.0f, 1);
}

static void draw_center_message(const char *title, uint32_t titleCol,
                                const char *line1, const char *line2)
{
    float s = G.scale;
    float px, py;
    panel(480 * s, 210 * s, &px, &py);
    draw_text_center(W * 0.5f, py + 28 * s, title, titleCol, 1.0f, 3);
    draw_text_center(W * 0.5f, py + 96 * s, line1, 0xf8fafc, 1.0f, 1);
    draw_text_center(W * 0.5f, py + 128 * s, line2, 0xa1a1aa, 1.0f, 1);
}

static void draw_state_overlay(void)
{
    char buf1[96], buf2[96];
    if (G.state == GS_TITLE) draw_title();
    else if (G.state == GS_CONTROLS) draw_controls();
    else if (G.state == GS_PAUSED)
        draw_center_message("PAUSED", 0xfacc15, "P / ESC resume", "R restart     Q quit");
    else if (G.state == GS_LEVEL_CLEAR) {
        snprintf(buf1, sizeof buf1, "LEVEL %d CLEAR   SCORE %d", G.level, G.score);
        snprintf(buf2, sizeof buf2, "ENTER next level     LIVES %d", G.lives);
        draw_center_message("CLEAR", 0x86efac, buf1, buf2);
    } else if (G.state == GS_BALL_LOST) {
        snprintf(buf1, sizeof buf1, "BALL LOST   LIVES %d", G.lives);
        draw_center_message("READY", 0xfacc15, buf1, "SPACE launch");
    } else if (G.state == GS_GAMEOVER) {
        snprintf(buf1, sizeof buf1, "FINAL SCORE %d", G.score);
        snprintf(buf2, sizeof buf2, "BEST %d     ENTER restart", G.highScore);
        draw_center_message("GAME OVER", 0xf87171, buf1, buf2);
    }
}

static void draw_flash(void)
{
    if (G.screenFlash <= 0.0f) return;
    int ai = (int)(G.screenFlash * 0.36f * 256);
    if (ai <= 0) return;
    if (ai > 256) ai = 256;
    uint32_t *p = canvas.px;
    for (size_t i = 0, n = (size_t)W * H; i < n; i++) {
        uint32_t c = p[i];
        int r = (c >> 16) & 255;
        int g = (c >> 8) & 255;
        int b = c & 255;
        r += ((255 - r) * ai) >> 8;
        g += ((240 - g) * ai) >> 8;
        b += ((210 - b) * ai) >> 8;
        p[i] = (c & 0xff000000u) |
               ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

/* repack the 0xAARRGGBB canvas into the R,G,B,A byte order the presenter
 * (kittyfb_present) and the PPM dumps consume */
static void repack_fb(void)
{
    const uint32_t *s = canvas.px;
    uint8_t *d = fb;
    for (size_t i = 0, n = (size_t)W * H; i < n; i++, d += 4) {
        uint32_t c = s[i];
        d[0] = (uint8_t)(c >> 16);
        d[1] = (uint8_t)(c >> 8);
        d[2] = (uint8_t)c;
        d[3] = (uint8_t)(c >> 24);
    }
}

void render_frame(void)
{
    if (!canvas.px || !backdrop.px || !fb) return;
    memcpy(canvas.px, backdrop.px, (size_t)W * H * 4);

    float shake = G.cameraShake;
    OX = (int)(sinf(G.frameCount * 12.989f) * shake);
    OY = (int)(cosf(G.frameCount * 9.173f) * shake);

    draw_stars();
    draw_grid();
    draw_playfield_frame();
    draw_bricks();
    draw_powerups();
    draw_particles();
    draw_paddle();
    draw_balls();

    OX = OY = 0;
    draw_hud();
    draw_state_overlay();
    draw_flash();
    repack_fb();
}
