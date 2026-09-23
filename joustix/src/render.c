#include "joustix.h"
#include "soft_raster.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *fb;
static sr_canvas canvas;
static int W, H;
static float scale_px, origin_x, origin_y, shake_x, shake_y;
static int clip_x0, clip_y0, clip_x1, clip_y1;
static Bitmap stage_img, gameover_img, rider_img[4], props_img, platform_img;

typedef struct { int x, y, w, h; bool ok; } SpriteBounds;
enum { RIDER_ATLAS_COLS = 2, RIDER_ATLAS_ROWS = 4, RIDER_POSES = 8 };
static SpriteBounds rider_bounds[4][RIDER_POSES], prop_bounds[4], platform_bounds;

typedef struct { char c; uint8_t row[7]; } Glyph;

/* Compact original 5x7 terminal-game font. */
static const Glyph glyphs[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}},      {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},   {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},     {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}},     {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}},     {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {'!',{4,4,4,4,4,0,4}},        {'?',{14,17,1,2,4,0,4}},
    {'.',{0,0,0,0,0,0,4}},        {',',{0,0,0,0,0,4,8}},
    {':',{0,4,0,0,4,0,0}},        {'-',{0,0,0,31,0,0,0}},
    {'/',{1,2,2,4,8,8,16}},       {'<',{2,4,8,16,8,4,2}},
    {'>',{8,4,2,1,2,4,8}},        {'+',{0,4,4,31,4,4,0}},
    {'=',{0,31,0,31,0,0,0}},      {'(',{2,4,8,8,8,4,2}},
    {')',{8,4,2,2,2,4,8}},        {'\'',{4,4,2,0,0,0,0}},
};

uint8_t *render_fb(void)
{
    if (fb != NULL)
        (void)sr_pack_rgba(&canvas, fb, (size_t)W * (size_t)H * 4u);
    return fb;
}

static float sx(float x) { return origin_x + shake_x + x * scale_px; }
static float sy(float y) { return origin_y + shake_y + y * scale_px; }

static void px_blend(int x, int y, uint32_t rgb, float a)
{
    sr_blend(&canvas, x, y, rgb, a);
}

static void px_set(int x, int y, uint32_t rgb)
{
    sr_px(&canvas, x, y, rgb);
}

static void rect_px(int x, int y, int w, int h, uint32_t rgb, float a)
{
    sr_fill_rect(&canvas, (float)x, (float)y, (float)w, (float)h, rgb, a);
}

static void circle_px(float cx, float cy, float r, uint32_t rgb, float a)
{
    sr_fill_circle(&canvas, cx, cy, r, rgb, a);
}

static void line_px(float x0, float y0, float x1, float y1, float width,
                    uint32_t rgb, float a)
{
    sr_line(&canvas, x0, y0, x1, y1, width, rgb, a, 0, 0);
}

static void rect_l(float x, float y, float w, float h, uint32_t c, float a)
{
    rect_px((int)sx(x), (int)sy(y), (int)ceilf(w * scale_px),
            (int)ceilf(h * scale_px), c, a);
}

static void circle_l(float x, float y, float r, uint32_t c, float a)
{
    circle_px(sx(x), sy(y), r * scale_px, c, a);
}

static void line_l(float x0, float y0, float x1, float y1, float width,
                   uint32_t c, float a)
{
    line_px(sx(x0), sy(y0), sx(x1), sy(y1), width * scale_px, c, a);
}

static const uint8_t *glyph_for(char c)
{
    static const uint8_t blank[7] = {0};
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++)
        if (glyphs[i].c == c) return glyphs[i].row;
    return blank;
}

static int text_width_px(const char *s, int size) { return (int)strlen(s) * 6 * size; }

static void text_px(int x, int y, const char *s, uint32_t c, float a, int size)
{
    for (; *s; s++, x += 6 * size) {
        const uint8_t *rows = glyph_for(*s);
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++)
                if (rows[gy] & (1u << (4 - gx)))
                    rect_px(x + gx * size, y + gy * size, size, size, c, a);
    }
}

static void text_l(float x, float y, const char *s, uint32_t c, float a, float mult)
{
    int size = (int)fmaxf(1.0f, floorf(scale_px * mult + 0.25f));
    text_px((int)sx(x), (int)sy(y), s, c, a, size);
}

static void text_center_l(float x, float y, const char *s, uint32_t c, float a,
                          float mult)
{
    int size = (int)fmaxf(1.0f, floorf(scale_px * mult + 0.25f));
    text_px((int)sx(x) - text_width_px(s, size) / 2, (int)sy(y), s, c, a, size);
}

static Bitmap load_ppm(const char *path)
{
    Bitmap image = {0};
    sr_canvas loaded;
    if (!sr_load_ppm(&loaded, path)) return image;
    if (loaded.w > 4096 || loaded.h > 4096) {
        sr_canvas_free(&loaded);
        return image;
    }
    image.w = loaded.w;
    image.h = loaded.h;
    image.px = loaded.px;
    image.ok = true;
    return image;
}

static void free_bitmap(Bitmap *image)
{
    free(image->px);
    memset(image, 0, sizeof *image);
}

static bool key_magenta(uint32_t rgb)
{
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    /* Include the bright antialiased fringe around the generated key while
     * preserving the darker violet/cyan palette inside the Shadow Lord. */
    return r > 135 && b > 150 && g < 100;
}

static SpriteBounds find_bounds(const Bitmap *image, int cols, int rows,
                                int col, int row)
{
    SpriteBounds out = {0};
    if (!image->ok || col < 0 || row < 0 || col >= cols || row >= rows) return out;
    int cell_w = image->w / cols, cell_h = image->h / rows;
    int min_x = cell_w, min_y = cell_h, max_x = -1, max_y = -1;
    int source_x = col * cell_w, source_y = row * cell_h;
    for (int y = 0; y < cell_h; y++)
        for (int x = 0; x < cell_w; x++) {
            uint32_t rgb = image->px[(source_y + y) * image->w + source_x + x];
            if (key_magenta(rgb)) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    if (max_x < min_x || max_y < min_y) return out;
    out.x = source_x + min_x;
    out.y = source_y + min_y;
    out.w = max_x - min_x + 1;
    out.h = max_y - min_y + 1;
    out.ok = true;
    return out;
}

static void cache_sprite_bounds(void)
{
    for (int set = 0; set < 4; set++)
        for (int cell = 0; cell < RIDER_POSES; cell++)
            rider_bounds[set][cell] = find_bounds(&rider_img[set],
                                                   RIDER_ATLAS_COLS,
                                                   RIDER_ATLAS_ROWS,
                                                   cell & 1, cell >> 1);
    for (int cell = 0; cell < 4; cell++)
        prop_bounds[cell] = find_bounds(&props_img, 2, 2, cell & 1, cell >> 1);
    platform_bounds = find_bounds(&platform_img, 1, 1, 0, 0);
}

static void draw_background(const Bitmap *image)
{
    if (!image->ok) return;
    int x0 = (int)sx(0), y0 = (int)sy(0);
    int width = (int)ceilf(LOGICAL_W * scale_px);
    int height = (int)ceilf(LOGICAL_H * scale_px);
    for (int dy = 0; dy < height; dy++) {
        int source_y = dy * image->h / height;
        for (int dx = 0; dx < width; dx++) {
            int source_x = dx * image->w / width;
            px_set(x0 + dx, y0 + dy, image->px[source_y * image->w + source_x]);
        }
    }
}

static void draw_sprite(const Bitmap *image, SpriteBounds bounds,
                        float center_x, float center_y, float max_w, float max_h,
                        float alpha, bool flip, bool bottom_anchor)
{
    if (!image->ok || !bounds.ok) return;
    float draw_w = max_w, draw_h = draw_w * bounds.h / bounds.w;
    if (draw_h > max_h) { draw_h = max_h; draw_w = draw_h * bounds.w / bounds.h; }
    int width = (int)fmaxf(1, roundf(draw_w * scale_px));
    int height = (int)fmaxf(1, roundf(draw_h * scale_px));
    int x0 = (int)roundf(sx(center_x) - width * .5f);
    int y0 = bottom_anchor ? (int)roundf(sy(center_y) - height)
                           : (int)roundf(sy(center_y) - height * .5f);
    for (int dy = 0; dy < height; dy++) {
        int source_y = bounds.y + dy * bounds.h / height;
        for (int dx = 0; dx < width; dx++) {
            int sample_x = flip ? width - 1 - dx : dx;
            int source_x = bounds.x + sample_x * bounds.w / width;
            uint32_t rgb = image->px[source_y * image->w + source_x];
            if (!key_magenta(rgb)) px_blend(x0 + dx, y0 + dy, rgb, alpha);
        }
    }
}

static void draw_stretched_keyed(const Bitmap *image, SpriteBounds bounds,
                                 float x, float y, float width_l, float height_l)
{
    if (!image->ok || !bounds.ok) return;
    int width = (int)fmaxf(1, roundf(width_l * scale_px));
    int height = (int)fmaxf(1, roundf(height_l * scale_px));
    int x0 = (int)roundf(sx(x)), y0 = (int)roundf(sy(y));
    for (int dy = 0; dy < height; dy++) {
        int source_y = bounds.y + dy * bounds.h / height;
        for (int dx = 0; dx < width; dx++) {
            int source_x = bounds.x + dx * bounds.w / width;
            uint32_t rgb = image->px[source_y * image->w + source_x];
            if (!key_magenta(rgb)) px_blend(x0 + dx, y0 + dy, rgb, 1);
        }
    }
}

static void draw_platform(const Platform *p)
{
    draw_stretched_keyed(&platform_img, platform_bounds,
                         p->x, p->y - .7f, p->w, 9.2f);
}

static void draw_lava(void)
{
    for (int x = 0; x < 320; x += 2) {
        float y = LAVA_TOP + sinf(x * .15f + G.scene_time * 3.2f) * 1.2f;
        line_l((float)x, y, x + 2.4f, y + sinf((x + 2) * .15f + G.scene_time * 3.2f),
               .8f, 0xffd45b, .82f);
    }
    for (int i = 0; i < 16; i++) {
        float phase = fmodf(G.scene_time * (1.4f + (i % 4) * .24f) + i * 1.73f, 4.0f);
        float x = 7.0f + fmodf(i * 43.7f, 306.0f);
        if (phase < 1.2f) circle_l(x, LAVA_TOP + 2.0f - phase * 2.0f,
                                  .7f + phase * .35f, 0xffd45b, 1.0f - phase * .55f);
    }
}

static void draw_troll(void)
{
    if (G.lava_troll_phase <= 0) return;
    float t = G.lava_troll_phase;
    float rise = t < 1.0f ? t : t < 1.75f ? 1.0f : fmaxf(0, (2.8f - t) / 1.05f);
    int pose = t > .78f && t < 2.05f ? 3 : 2;
    float bottom = LAVA_TOP + 13.5f - rise * 13.0f;
    draw_sprite(&props_img, prop_bounds[pose], G.lava_troll_x, bottom,
                28.0f, 29.0f, 1, false, true);
}

static void draw_rider(const Rider *r, bool player, int type)
{
    if (!r->active) return;
    if (r->spawn_timer > 0) {
        float t = r->spawn_timer;
        if (((int)(t * 18)) & 1) return;
        for (int i = 0; i < 3; i++)
            circle_l(r->x + 9, r->y + 7, 5 + i * 3 + t * 3,
                     player ? 0xb7f3ff : 0xc7f9b7, .10f);
    }
    int enemy_set = type < 0 ? 0 : type >= EN_TYPE_COUNT ? EN_TYPE_COUNT - 1 : type;
    int set = player ? 0 : enemy_set + 1;
    int pose;
    if (r->on_platform) {
        /* Frames 0-3 are a four-step ground cycle. Deriving the phase from
         * distance traveled keeps the feet still when the rider stops and
         * naturally reverses the cycle when the rider turns around. */
        pose = fabsf(r->vx) > 2.0f
             ? ((int)floorf((r->x + LOGICAL_W + 32.0f) * .18f) & 3)
             : 1;
    } else {
        /* Frames 4-7 are glide, wing-up, wing-down, and dive. */
        pose = r->flap_anim > .12f ? 5 : r->flap_anim > 0 ? 6
                                                    : r->vy > 35 ? 7 : 4;
    }
    draw_sprite(&rider_img[set], rider_bounds[set][pose],
                r->x + 9, r->y + 13.0f, 24.8f, 16.8f, 1, r->dir < 0, true);
    if (player && r->invuln > 0 && r->spawn_timer <= 0) {
        float a = .18f + .18f * sinf(G.scene_time * 15.0f);
        circle_l(r->x + 9, r->y + 6.5f, 10, 0x9ceeff, a * .35f);
    }
}

static void draw_egg(const Egg *e)
{
    if (!e->active) return;
    int pose = e->hatch_timer < 2.0f && ((int)(e->hatch_timer * 7) & 1) ? 1 : 0;
    draw_sprite(&props_img, prop_bounds[pose], e->x + 2.5f, e->y + 6.0f,
                6.0f, 7.0f, 1, false, true);
}

static void draw_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        float a = clampf(p->life / p->max_life, 0, 1);
        circle_l(p->x, p->y, .45f + .6f * a, p->color, a);
    }
}

static void draw_hud(void)
{
    rect_l(0, 0, LOGICAL_W, 14, 0x050814, .64f);
    line_l(0, 14, 320, 14, .45f, 0x59657e, .75f);
    char b[64];
    snprintf(b, sizeof b, "SCORE %06d", G.score);
    text_l(5, 4.2f, b, 0xf8fafc, 1, .70f);
    snprintf(b, sizeof b, "WAVE %d", G.wave);
    text_center_l(160, 4.2f, b, 0xfacc15, 1, .70f);
    snprintf(b, sizeof b, "LIVES %d", G.lives);
    int size = (int)fmaxf(1.0f, floorf(scale_px * .70f + .25f));
    text_px((int)sx(315) - text_width_px(b, size), (int)sy(4.2f), b,
            G.lives > 1 ? 0xb9f6c7 : 0xff7770, 1, size);
    if (!G.sound_on) text_l(5, 17, "SOUND OFF", 0x94a3b8, .85f, .55f);
}

static void panel(float x, float y, float w, float h)
{
    rect_l(x + 1.5f, y + 1.5f, w, h, 0x000000, .45f);
    rect_l(x, y, w, h, 0x080d1d, .92f);
    line_l(x, y, x + w, y, .8f, 0xe0b752, 1);
    line_l(x, y + h, x + w, y + h, .8f, 0x5e4725, 1);
    line_l(x, y, x, y + h, .8f, 0xa27d38, 1);
    line_l(x + w, y, x + w, y + h, .8f, 0xa27d38, 1);
}

static void draw_game_scene(void)
{
    for (int i = 0; i < PLATFORM_COUNT; i++) draw_platform(&G.platforms[i]);
    draw_lava();
    draw_troll();
    for (int i = 0; i < MAX_EGGS; i++) draw_egg(&G.eggs[i]);
    for (int i = 0; i < MAX_ENEMIES; i++)
        draw_rider(&G.enemies[i].rider, false, G.enemies[i].type);
    draw_rider(&G.player, true, 0);
    draw_particles();
    draw_hud();
    if (G.message_timer > 0 && G.message[0] &&
        G.state != GS_PAUSED && G.state != GS_GAMEOVER) {
        float a = clampf(G.message_timer * 2, 0, 1);
        text_center_l(160, 19, G.message, 0xffdf61, a, 1.05f);
    }
}

static void draw_title(void)
{
    Platform p1 = { 18, 143, 84, 5 }, p2 = { 218, 143, 84, 5 };
    draw_platform(&p1); draw_platform(&p2); draw_lava();
    Rider a = { .x = 52 + sinf(G.scene_time * .8f) * 8, .y = 102 + sinf(G.scene_time * 1.4f) * 7,
                .dir = 1, .active = true, .flap_anim = fmodf(G.scene_time, .5f) < .2f ? .18f : 0 };
    Rider b = { .x = 245 - sinf(G.scene_time * .7f) * 10, .y = 89 + cosf(G.scene_time * 1.2f) * 8,
                .dir = -1, .active = true, .flap_anim = fmodf(G.scene_time + .2f, .55f) < .2f ? .18f : 0 };
    draw_rider(&a, true, 0); draw_rider(&b, false, EN_BOUNDER);
    rect_l(42, 22, 216, 105, 0x030714, .72f);
    text_center_l(160.8f, 32.8f, "JOUSTIX", 0x4b2808, .85f, 3.05f);
    text_center_l(160, 31, "JOUSTIX", 0xffd84d, 1, 3.05f);
    text_center_l(160, 60, "FLY HIGH. STRIKE HIGHER.", 0xc9d8f2, .92f, .75f);
    panel(91, 75, 138, 37);
    text_center_l(160, 80, "DIFFICULTY", 0x94a3b8, 1, .62f);
    static const char *names[] = { "< SQUIRE >", "< KNIGHT >", "< BLACK KNIGHT >" };
    text_center_l(160, 91, names[G.difficulty], 0xf8fafc, 1, .84f);
    char best[32];
    snprintf(best, sizeof best, "BEST %06d", G.high_score);
    text_center_l(160, 104, best, 0x94a3b8, 1, .50f);
    float blink = .55f + .45f * sinf(G.scene_time * 3.5f);
    text_center_l(160, 119, "PRESS ENTER TO RIDE", 0xffe888, blink, .78f);
    text_center_l(160, 133, "LEFT RIGHT CHOOSE   M SOUND   Q QUIT", 0x94a3b8, .9f, .51f);
}

static void draw_overlay(void)
{
    if (G.state == GS_PAUSED) {
        panel(88, 66, 144, 51);
        text_center_l(160, 75, "PAUSED", 0xffdb58, 1, 1.45f);
        text_center_l(160, 98, "P OR ESC TO RESUME", 0xcbd5e1, 1, .62f);
    } else if (G.state == GS_GAMEOVER) {
        rect_l(0, 0, 320, 180, 0x280506, .18f);
        panel(74, 33, 172, 114);
        text_center_l(160, 42, "GAME OVER", 0xff625b, 1, 1.55f);
        char b[64];
        snprintf(b, sizeof b, "SCORE %06d", G.score);
        text_center_l(160, 68, b, 0xf8fafc, 1, .64f);
        snprintf(b, sizeof b, "BEST  %06d", G.high_score);
        text_center_l(160, 80, b, 0xcbd5e1, 1, .60f);
        bool restart = G.gameover_choice == GAMEOVER_RESTART;
        text_center_l(160, 99, restart ? "> RIDE AGAIN <" : "RIDE AGAIN",
                      restart ? 0xffdd67 : 0x94a3b8, 1, .70f);
        text_center_l(160, 114, restart ? "MAIN MENU" : "> MAIN MENU <",
                      restart ? 0x94a3b8 : 0xffdd67, 1, .70f);
        text_center_l(160, 135, "ARROWS CHOOSE   ENTER SELECT",
                      0x94a3b8, 1, .46f);
    }
}

void render_init(int w, int h)
{
    fb = NULL;
    W = H = 0;
    stage_img = load_ppm(asset_path("stage.ppm"));
    gameover_img = load_ppm(asset_path("gameover.ppm"));
    rider_img[0] = load_ppm(asset_path("player.ppm"));
    rider_img[1] = load_ppm(asset_path("bounder.ppm"));
    rider_img[2] = load_ppm(asset_path("hunter.ppm"));
    rider_img[3] = load_ppm(asset_path("shadow.ppm"));
    props_img = load_ppm(asset_path("props.ppm"));
    platform_img = load_ppm(asset_path("platform.ppm"));
    cache_sprite_bounds();
    render_resize(w, h);
}

void render_resize(int w, int h)
{
    W = w; H = h;
    uint8_t *next = realloc(fb, (size_t)W * H * 4);
    if (!next) { free(fb); fb = NULL; return; }
    fb = next;
    sr_canvas_free(&canvas);
    if (!sr_canvas_init(&canvas, W, H)) { free(fb); fb = NULL; return; }
    scale_px = fminf(W / LOGICAL_W, H / LOGICAL_H);
    origin_x = (W - LOGICAL_W * scale_px) * .5f;
    origin_y = (H - LOGICAL_H * scale_px) * .5f;
    clip_x0 = (int)floorf(origin_x);
    clip_y0 = (int)floorf(origin_y);
    clip_x1 = (int)ceilf(origin_x + LOGICAL_W * scale_px);
    clip_y1 = (int)ceilf(origin_y + LOGICAL_H * scale_px);
    sr_canvas_set_clip(&canvas, clip_x0, clip_y0,
                       clip_x1 - clip_x0, clip_y1 - clip_y0);
}

void render_shutdown(void)
{
    free_bitmap(&stage_img);
    free_bitmap(&gameover_img);
    for (int i = 0; i < 4; i++) free_bitmap(&rider_img[i]);
    free_bitmap(&props_img);
    free_bitmap(&platform_img);
    free(fb); fb = NULL; W = H = 0;
    sr_canvas_free(&canvas);
}

static bool expect_bitmap(const Bitmap *image, const char *name, int width, int height,
                          char *error, size_t error_len)
{
    if (!image->ok) {
        snprintf(error, error_len, "required production image %s is missing or corrupt", name);
        return false;
    }
    if (image->w != width || image->h != height) {
        snprintf(error, error_len, "production image %s is %dx%d; expected %dx%d",
                 name, image->w, image->h, width, height);
        return false;
    }
    return true;
}

bool render_validate_assets(char *error, size_t error_len)
{
    if (!expect_bitmap(&stage_img, "stage.ppm", 640, 360, error, error_len) ||
        !expect_bitmap(&gameover_img, "gameover.ppm", 640, 360, error, error_len) ||
        !expect_bitmap(&rider_img[0], "player.ppm", 512, 768, error, error_len) ||
        !expect_bitmap(&rider_img[1], "bounder.ppm", 512, 768, error, error_len) ||
        !expect_bitmap(&rider_img[2], "hunter.ppm", 512, 768, error, error_len) ||
        !expect_bitmap(&rider_img[3], "shadow.ppm", 512, 768, error, error_len) ||
        !expect_bitmap(&props_img, "props.ppm", 512, 512, error, error_len) ||
        !expect_bitmap(&platform_img, "platform.ppm", 640, 128, error, error_len))
        return false;
    for (int set = 0; set < 4; set++)
        for (int cell = 0; cell < RIDER_POSES; cell++)
            if (!rider_bounds[set][cell].ok) {
                snprintf(error, error_len, "rider atlas %d has an empty pose %d", set, cell);
                return false;
            }
    for (int cell = 0; cell < 4; cell++)
        if (!prop_bounds[cell].ok) {
            snprintf(error, error_len, "props.ppm has an empty cell %d", cell);
            return false;
        }
    if (!platform_bounds.ok) {
        snprintf(error, error_len, "platform.ppm contains no keyed sprite");
        return false;
    }
    error[0] = '\0';
    return true;
}

void render_frame(void)
{
    if (!fb) return;
    clip_x0 = clip_y0 = 0; clip_x1 = W; clip_y1 = H;
    sr_canvas_reset_clip(&canvas);
    sr_clear(&canvas, 0x010207u);
    clip_x0 = (int)floorf(origin_x); clip_y0 = (int)floorf(origin_y);
    clip_x1 = (int)ceilf(origin_x + LOGICAL_W * scale_px);
    clip_y1 = (int)ceilf(origin_y + LOGICAL_H * scale_px);
    sr_canvas_set_clip(&canvas, clip_x0, clip_y0,
                       clip_x1 - clip_x0, clip_y1 - clip_y0);
    if (G.shake > 0) {
        float strength = G.shake * 3.2f;
        shake_x = sinf(G.ticks * 2.17f) * strength * scale_px;
        shake_y = cosf(G.ticks * 1.61f) * strength * .55f * scale_px;
    } else shake_x = shake_y = 0;
    if (G.state == GS_GAMEOVER) {
        draw_background(&gameover_img);
        draw_overlay();
    } else {
        draw_background(&stage_img);
        if (G.state == GS_TITLE) draw_title();
        else { draw_game_scene(); draw_overlay(); }
    }
    if (G.flash > 0) rect_l(0, 0, 320, 180, 0xffd27a, G.flash * 1.8f);
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&canvas, path);
}
