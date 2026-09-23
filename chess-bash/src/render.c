#include "chess_bash.h"
#include "soft_raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *fb;
static sr_canvas canvas;
static int W, H;
static Bitmap title_img, board_bg_img, piece_atlas, piece_direction_atlas;
static Bitmap fight_atlas, effect_atlas;
static int loaded_theme = -1;
static float board_cx, board_top, tile_w, tile_h;
static float shake_x, shake_y;   /* whole-scene impact shake, set per frame */

uint8_t *render_fb(void)
{
    if (fb != NULL)
        (void)sr_pack_rgba(&canvas, fb, (size_t)W * (size_t)H * 4u);
    return fb;
}

/* ---------- board themes ---------- */

static const struct { const char *file; const char *name; } themes[BOARD_THEME_COUNT] = {
    { "ivy-courtyard",   "Ivy Courtyard" },
    { "marble-terminal", "Marble Terminal" },
    { "desert-ruins",    "Desert Ruins" },
    { "ice-cathedral",   "Ice Cathedral" },
    { "volcanic-forge",  "Volcanic Forge" },
    { "orbital-space",   "Orbital Space" },
};

const char *board_theme_name(int i)
{
    return themes[((i % BOARD_THEME_COUNT) + BOARD_THEME_COUNT) % BOARD_THEME_COUNT].name;
}

/* ---------- primitives ---------- */

static uint32_t rgb_mix(uint32_t a, uint32_t b, float t)
{
    return sr_mix(a, b, t);
}

static void px_set(int x, int y, uint32_t rgb)
{
    sr_px(&canvas, x, y, rgb);
}

static void px_blend(int x, int y, uint32_t rgb, float a)
{
    sr_blend(&canvas, x, y, rgb, a);
}

static void fill_rect(int x0, int y0, int w, int h, uint32_t rgb, float a)
{
    sr_fill_rect(&canvas, (float)x0, (float)y0, (float)w, (float)h, rgb, a);
}

static void fill_circle(float cx, float cy, float r, uint32_t rgb, float a)
{
    sr_fill_circle(&canvas, cx, cy, r, rgb, a);
}

static void draw_line(float x0, float y0, float x1, float y1, float width,
                      uint32_t rgb, float a)
{
    sr_line(&canvas, x0, y0, x1, y1, width, rgb, a, 0, 0);
}

static void fill_diamond(float cx, float cy, float tw, float th, uint32_t rgb, float a)
{
    float xs[4] = {cx, cx + tw * 0.5f, cx, cx - tw * 0.5f};
    float ys[4] = {cy - th * 0.5f, cy, cy + th * 0.5f, cy};
    sr_fill_convex(&canvas, xs, ys, 4u, rgb, a);
}

static void fill_triangle(float ax, float ay, float bx, float by, float cx, float cy,
                          uint32_t rgb, float a)
{
    sr_fill_triangle(&canvas, ax, ay, bx, by, cx, cy, rgb, a);
}

static void outline_diamond(float cx, float cy, float tw, float th, uint32_t rgb, float a)
{
    float x[4] = { cx, cx + tw * 0.5f, cx, cx - tw * 0.5f };
    float y[4] = { cy - th * 0.5f, cy, cy + th * 0.5f, cy };
    for (int i = 0; i < 4; i++)
        draw_line(x[i], y[i], x[(i + 1) & 3], y[(i + 1) & 3], 2.0f, rgb, a);
}

static int text_width(const char *s, int scale)
{
    return sr_text_width(s, scale);
}

static void draw_text(float fx, float fy, const char *s, uint32_t rgb, float a, int scale)
{
    sr_text(&canvas, fx, fy, s, rgb, a, scale);
}

static void draw_text_center(float cx, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{
    draw_text(cx - text_width(s, scale) / 2.0f, y, s, rgb, a, scale);
}

/* drop-shadowed display text for titles/banners */
static void draw_text_banner(float cx, float y, const char *s,
                             uint32_t rgb, float a, int scale)
{
    draw_text_center(cx + scale, y + scale, s, 0x000000, a * 0.85f, scale);
    draw_text_center(cx, y, s, rgb, a, scale);
}

static Bitmap load_ppm(const char *path)
{
    Bitmap b = {0};
    sr_canvas image;
    if (!sr_load_ppm(&image, path)) return b;
    if (image.w > 8192 || image.h > 8192) {
        sr_canvas_free(&image);
        return b;
    }
    b.w = image.w;
    b.h = image.h;
    b.px = image.px;
    b.ok = true;
    return b;
}

static void free_bitmap(Bitmap *b)
{
    free(b->px);
    memset(b, 0, sizeof *b);
}

void render_set_theme(int i)
{
    i = ((i % BOARD_THEME_COUNT) + BOARD_THEME_COUNT) % BOARD_THEME_COUNT;
    if (i == loaded_theme && board_bg_img.ok) return;
    free_bitmap(&board_bg_img);
    char rel[128];
    snprintf(rel, sizeof rel, "boards/%s.ppm", themes[i].file);
    board_bg_img = load_ppm(asset_path(rel));
    loaded_theme = i;
}

static void draw_bitmap_cover_zoom(const Bitmap *b, float zoom)
{
    if (!b->ok) return;
    float scale = fmaxf((float)W / b->w, (float)H / b->h) * zoom;
    float ox = (W - b->w * scale) * 0.5f;
    float oy = (H - b->h * scale) * 0.5f;
    for (int y = 0; y < H; y++) {
        int sy = (int)((y - oy) / scale);
        if (sy < 0) sy = 0;
        if (sy >= b->h) sy = b->h - 1;
        for (int x = 0; x < W; x++) {
            int sx = (int)((x - ox) / scale);
            if (sx < 0) sx = 0;
            if (sx >= b->w) sx = b->w - 1;
            px_set(x, y, b->px[sy * b->w + sx]);
        }
    }
}

static void draw_bitmap_cover(const Bitmap *b)
{
    draw_bitmap_cover_zoom(b, 1.0f);
}

/* ---------- sprite sampling ---------- */

static bool key_green(uint32_t rgb)
{
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    return g > 95 && g > r * 13 / 8 && g > b * 13 / 8;
}

/* soften the chroma-key fringe: pixels that lean green (anti-aliased edges
 * against the key color) get their green pulled down to the other channels */
static uint32_t degreen(uint32_t rgb)
{
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int hi = r > b ? r : b;
    if (g > 60 && g > hi + hi / 4) {
        g = hi + hi / 8;
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    return rgb;
}

static void draw_atlas_cell(const Bitmap *b, int cols, int rows, int col, int row,
                            float cx, float by, float size, float alpha, bool flip)
{
    if (!b->ok || col < 0 || row < 0 || col >= cols || row >= rows) return;
    int sw = b->w / cols;
    int sh = b->h / rows;
    int dw = (int)size;
    int dh = (int)size;
    int dx0 = (int)(cx - dw * 0.5f);
    int dy0 = (int)(by - dh * 0.86f);
    for (int dy = 0; dy < dh; dy++) {
        int sy = row * sh + dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sample_dx = flip ? (dw - 1 - dx) : dx;
            int sx = col * sw + sample_dx * sw / dw;
            uint32_t rgb = b->px[sy * b->w + sx];
            if (key_green(rgb)) continue;
            px_blend(dx0 + dx, dy0 + dy, degreen(rgb), alpha);
        }
    }
}

static bool atlas_cell_bounds(const Bitmap *b, int cols, int rows, int col, int row,
                              int *sx0, int *sy0, int *minx, int *miny,
                              int *bw, int *bh)
{
    if (!b->ok || col < 0 || row < 0 || col >= cols || row >= rows)
        return false;
    int sw = b->w / cols;
    int sh = b->h / rows;
    *sx0 = col * sw;
    *sy0 = row * sh;
    *minx = sw;
    *miny = sh;
    int maxx = -1, maxy = -1;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint32_t rgb = b->px[(*sy0 + y) * b->w + *sx0 + x];
            if (key_green(rgb)) continue;
            if (x < *minx) *minx = x;
            if (x > maxx) maxx = x;
            if (y < *miny) *miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (maxx < *minx || maxy < *miny) return false;
    *bw = maxx - *minx + 1;
    *bh = maxy - *miny + 1;
    return *bw > 0 && *bh > 0;
}

static void piece_draw_size(int bw, int bh, float size, float *dwf, float *dhf)
{
    *dhf = size;
    *dwf = *dhf * bw / (float)bh;
    if (*dwf > size * 1.10f) {
        *dwf = size * 1.10f;
        *dhf = *dwf * bh / (float)bw;
    }
}

static void draw_sprite_cell_pose(const Bitmap *b, int cols, int rows, int col, int row,
                                  float cx, float by, float size,
                                  float alpha, bool flip, float angle)
{
    int sx0, sy0, minx, miny, bw, bh;
    if (!atlas_cell_bounds(b, cols, rows, col, row, &sx0, &sy0, &minx, &miny, &bw, &bh))
        return;

    float dwf, dhf;
    piece_draw_size(bw, bh, size, &dwf, &dhf);
    if (dwf < 1 || dhf < 1) return;

    float ca = cosf(angle), sa = sinf(angle);
    float corners[4][2] = {
        { -dwf * 0.5f, -dhf }, { dwf * 0.5f, -dhf },
        {  dwf * 0.5f,  0.0f }, { -dwf * 0.5f, 0.0f }
    };
    float min_dx = 1e9f, min_dy = 1e9f, max_dx = -1e9f, max_dy = -1e9f;
    for (int i = 0; i < 4; i++) {
        float rx = corners[i][0] * ca - corners[i][1] * sa;
        float ry = corners[i][0] * sa + corners[i][1] * ca;
        if (rx < min_dx) min_dx = rx;
        if (rx > max_dx) max_dx = rx;
        if (ry < min_dy) min_dy = ry;
        if (ry > max_dy) max_dy = ry;
    }

    int x0 = (int)floorf(cx + min_dx) - 2;
    int x1 = (int)ceilf(cx + max_dx) + 2;
    int y0 = (int)floorf(by + min_dy) - 2;
    int y1 = (int)ceilf(by + max_dy) + 2;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f - cx;
            float py = y + 0.5f - by;
            float lx = px * ca + py * sa;
            float ly = -px * sa + py * ca;
            if (lx < -dwf * 0.5f || lx >= dwf * 0.5f || ly < -dhf || ly >= 0.0f)
                continue;
            int dx = (int)((lx + dwf * 0.5f) * bw / dwf);
            int dy = (int)((ly + dhf) * bh / dhf);
            if (dx < 0) dx = 0;
            if (dx >= bw) dx = bw - 1;
            if (dy < 0) dy = 0;
            if (dy >= bh) dy = bh - 1;
            int sample_dx = flip ? (bw - 1 - dx) : dx;
            int sx = sx0 + minx + sample_dx;
            int sy = sy0 + miny + dy;
            uint32_t rgb = b->px[sy * b->w + sx];
            if (key_green(rgb)) continue;
            px_blend(x, y, degreen(rgb), alpha);
        }
    }
}

static void draw_piece_shadow(int side, float cx, float by, float size,
                              float alpha, float angle)
{
    fill_circle(cx, by - size * 0.08f, size * (fabsf(angle) > 0.45f ? 0.32f : 0.25f),
                side == SIDE_WHITE ? 0x5b1412 : 0x073d54, 0.22f * alpha);
}

static void draw_piece_sprite_pose(char piece, float cx, float by, float size,
                                   float alpha, bool flip, float angle)
{
    int type = piece_type(piece);
    int side = piece_color(piece);
    if (type < 0 || side < 0) return;
    draw_piece_shadow(side, cx, by, size, alpha, angle);
    draw_sprite_cell_pose(&piece_atlas, 6, 2, type, side, cx, by, size, alpha, flip, angle);
}

static int direction_row_for(int side, bool away)
{
    if (side == SIDE_WHITE) return away ? 1 : 0;
    return away ? 3 : 2;
}

static void draw_piece_direction_pose(char piece, float cx, float by, float size,
                                      float alpha, bool away, bool flip, float angle)
{
    int type = piece_type(piece);
    int side = piece_color(piece);
    if (type < 0 || side < 0) return;
    if (!piece_direction_atlas.ok) {
        draw_piece_sprite_pose(piece, cx, by, size, alpha, flip, angle);
        return;
    }
    draw_piece_shadow(side, cx, by, size, alpha, angle);
    draw_sprite_cell_pose(&piece_direction_atlas, 6, 4, type, direction_row_for(side, away),
                          cx, by, size, alpha, flip, angle);
}

static bool motion_flip_for_piece(char piece, bool away)
{
    int side = piece_color(piece);
    if (side < 0) return false;
    if (side == SIDE_WHITE) return away;
    return !away;
}

static void draw_piece_static(char piece, float cx, float by, float size, float alpha)
{
    int side = piece_color(piece);
    if (side < 0) return;
    int near_side = game_view_flipped() ? SIDE_BLACK : SIDE_WHITE;
    bool away = side == near_side;
    draw_piece_direction_pose(piece, cx, by, size, alpha, away,
                              motion_flip_for_piece(piece, away), 0.0f);
}

static bool motion_away_for_delta(char piece, float dy)
{
    if (dy < -tile_h * 0.10f) return true;
    if (dy > tile_h * 0.10f) return false;
    return piece_color(piece) == (game_view_flipped() ? SIDE_BLACK : SIDE_WHITE);
}

/* ---------- fight atlas poses ----------
 *
 * pieces_fight.ppm is the identity-locked v4 battle atlas: it was generated
 * with the board sprites as exact character references, so a piece keeps
 * its armor, mount, hair and palette when it fights. 6 cols (P N B R Q K)
 * x 8 rows: rows 0-3 red (back/away view, matching how red stands on the
 * board), rows 4-7 blue (front view); per side: ready, windup, strike,
 * recover. (The companion walk atlases remain unused: every generation so
 * far broke view or identity consistency, so marches animate the board
 * pose procedurally instead.)
 * Cells are not horizontally facing-consistent, so each carries its native
 * facing from a visual audit: -1 left, +1 right, 0 front/neutral. */

static const int8_t fight_facing[8][6] = {
    {  0,  0,  0, 0, 0,  0 },
    { -1, -1,  0, 0, 0,  0 },
    { -1,  1,  1, 0, 1, -1 },
    {  0,  0,  0, 0, 0,  0 },
    {  0,  0,  0, 0, 0,  0 },
    { -1, -1,  0, 0, 0,  0 },
    { -1,  1, -1, 0, 1,  1 },
    {  0,  0,  0, 0, 0,  0 },
};

static bool cell_flip(const int8_t table[8][6], int row, int type,
                      int dirx, bool fallback_flip)
{
    int8_t f = table[row & 7][type];
    if (f == 0) return fallback_flip;
    return (dirx >= 0 ? 1 : -1) != f;
}

/* one animated sprite, sortable into the painter's sweep */
typedef struct {
    bool used;
    char piece;
    float x, y;          /* feet anchor */
    float size;
    float alpha;
    float angle;
    float depth;         /* board diagonal (file + visual_row) equivalent */
    int atlas;           /* 0 = direction pose, 2 = fight cell */
    int cell_row;        /* for walk/fight atlases */
    bool away;           /* for direction poses */
    bool flip;
} AnimSprite;

#define MAX_ANIM_SPRITES 4
static AnimSprite anim_sprites[MAX_ANIM_SPRITES];

static void draw_anim_sprite(const AnimSprite *s)
{
    if (!s->used) return;
    int type = piece_type(s->piece);
    int side = piece_color(s->piece);
    if (type < 0 || side < 0) return;
    if (s->atlas == 2 && fight_atlas.ok) {
        draw_piece_shadow(side, s->x, s->y, s->size, s->alpha, s->angle);
        draw_sprite_cell_pose(&fight_atlas, 6, 8, type, s->cell_row,
                              s->x, s->y, s->size, s->alpha, s->flip, s->angle);
    } else {
        draw_piece_direction_pose(s->piece, s->x, s->y, s->size, s->alpha,
                                  s->away, s->flip, s->angle);
    }
}

/* marching pose: one view-stable directional sprite, held for the whole
 * move so the piece never appears to spin; the walking reads through the
 * procedural bob/lean/footfall the callers add on top */
static void set_march_pose(AnimSprite *s, char piece, float dys)
{
    bool away = motion_away_for_delta(piece, dys);
    s->atlas = 0;
    s->away = away;
    s->flip = motion_flip_for_piece(piece, away);
}

/* settle facing: the pose every piece holds when standing on the board;
 * marches ease into it over the last stretch so arrival doesn't snap */
static void set_static_pose(AnimSprite *s, char piece)
{
    bool away = piece_color(piece) ==
                (game_view_flipped() ? SIDE_BLACK : SIDE_WHITE);
    s->atlas = 0;
    s->away = away;
    s->flip = motion_flip_for_piece(piece, away);
}

static void set_fight_pose(AnimSprite *s, char piece, int pose_row, int dirx)
{
    int side = piece_color(piece);
    int type = piece_type(piece);
    int row = (side == SIDE_WHITE ? 0 : 4) + pose_row;
    if (!fight_atlas.ok) {
        s->atlas = 0;
        s->away = motion_away_for_delta(piece, 0);
        s->flip = motion_flip_for_piece(piece, s->away);
        return;
    }
    s->atlas = 2;
    s->cell_row = row;
    s->flip = cell_flip(fight_facing, row, type, dirx, false);
}

/* ---------- effects ---------- */

static void draw_effect(int effect, float cx, float cy, float size, float alpha)
{
    if (effect < 0) return;
    int col = effect % 5;
    int row = effect / 5;
    draw_atlas_cell(&effect_atlas, 5, 3, col, row, cx, cy + size * 0.35f, size,
                    alpha, false);
}

static float ease_smooth(float t)
{
    t = clampf(t, 0, 1);
    return t * t * (3.0f - 2.0f * t);
}

static float pulse_window(float t, float a, float b)
{
    if (t <= a || t >= b) return 0.0f;
    float u = (t - a) / (b - a);
    return sinf(u * 3.14159265f);
}

static void draw_spark_burst(float cx, float cy, float r, uint32_t color, float a)
{
    for (int i = 0; i < 10; i++) {
        float ang = i * 0.6283185f + G.frame_count * 0.07f;
        float r0 = r * 0.18f;
        float r1 = r * (0.56f + 0.16f * ((i * 17) & 3));
        draw_line(cx + cosf(ang) * r0, cy + sinf(ang) * r0,
                  cx + cosf(ang) * r1, cy + sinf(ang) * r1,
                  fmaxf(1.5f, r * 0.035f), color, a);
    }
    fill_circle(cx, cy, r * 0.14f, 0xffffff, a);
}

static void draw_piece_attack_trail(int attacker_type, float ax, float ay,
                                    float tx, float ty, int dir, uint32_t color, float a)
{
    if (a <= 0) return;
    switch (attacker_type) {
    case PT_PAWN:
        draw_line(ax + dir * tile_w * 0.18f, ay - tile_w * 0.70f,
                  tx - dir * tile_w * 0.28f, ty - tile_w * 0.72f,
                  4.0f, 0xf8fafc, a);
        break;
    case PT_KNIGHT:
        for (int i = 0; i < 5; i++) {
            float k = i / 4.0f;
            fill_circle(ax - dir * tile_w * (0.18f + k * 0.34f),
                        ay - tile_w * (0.08f + k * 0.03f),
                        tile_w * (0.05f + k * 0.025f), 0xb08968, a * (1.0f - k * 0.65f));
        }
        break;
    case PT_BISHOP:
        draw_line(ax + dir * tile_w * 0.05f, ay - tile_w * 0.95f,
                  tx - dir * tile_w * 0.18f, ty - tile_w * 0.74f,
                  7.0f, color, a * 0.72f);
        draw_line(ax + dir * tile_w * 0.05f, ay - tile_w * 0.95f,
                  tx - dir * tile_w * 0.18f, ty - tile_w * 0.74f,
                  2.0f, 0xffffff, a);
        break;
    case PT_ROOK:
        for (int i = 0; i < 4; i++)
            fill_diamond(tx - dir * tile_w * (0.15f + i * 0.12f),
                         ty - tile_w * (0.05f + (i & 1) * 0.08f),
                         tile_w * 0.18f, tile_h * 0.22f, 0x6b7280, a * 0.65f);
        break;
    case PT_QUEEN:
        for (int i = 0; i < 3; i++) {
            float rr = tile_w * (0.32f + i * 0.16f);
            outline_diamond(tx, ty - tile_w * 0.62f, rr, rr * 0.48f,
                            i == 1 ? 0xfef08a : color, a * (0.85f - i * 0.18f));
        }
        break;
    case PT_KING:
        draw_line(ax + dir * tile_w * 0.10f, ay - tile_w * 0.95f,
                  tx - dir * tile_w * 0.20f, ty - tile_w * 0.36f,
                  6.0f, 0xf8fafc, a);
        draw_line(ax + dir * tile_w * 0.17f, ay - tile_w * 0.82f,
                  tx - dir * tile_w * 0.08f, ty - tile_w * 0.25f,
                  2.0f, 0xfacc15, a);
        break;
    default:
        draw_line(ax, ay - tile_w * 0.60f, tx, ty - tile_w * 0.60f, 4.0f, color, a);
        break;
    }
}

/* ---------- board ---------- */

static void clear_bg(uint32_t top, uint32_t bottom)
{
    for (int y = 0; y < H; y++) {
        float t = (float)y / (H - 1);
        uint32_t c = rgb_mix(top, bottom, t);
        for (int x = 0; x < W; x++)
            px_set(x, y, c);
    }
}

static int hud_top_height(void)
{
    return H < 560 ? 44 : 66;
}

static void setup_board_metrics(void)
{
    tile_w = fminf(W / 10.5f, (H - hud_top_height() - 48) / 6.0f);
    tile_w = clampf(tile_w, 40.0f, 116.0f);
    tile_h = tile_w * 0.48f;
    board_cx = W * 0.50f;
    /* The diamond spans board_top + 0.5..8.5 tile heights, so placing its
     * midpoint at H/2 centers the actual game board in the background. */
    board_top = H * 0.50f - tile_h * 4.50f;
}

static void square_center(int sq, float *cx, float *cy)
{
    int f = sq_file(sq);
    int r = sq_rank(sq);
    int visual_f = game_view_flipped() ? 7 - f : f;
    int visual_r = game_view_flipped() ? r : 7 - r;
    *cx = board_cx + (visual_f - visual_r) * tile_w * 0.5f + shake_x;
    *cy = board_top + (visual_f + visual_r) * tile_h * 0.5f + tile_h + shake_y;
}

static float depth_from_screen_y(float cy)
{
    return (cy - shake_y - board_top - tile_h) / (tile_h * 0.5f);
}

static void draw_square_overlay(int sq, uint32_t rgb, float alpha)
{
    float cx, cy;
    square_center(sq, &cx, &cy);
    fill_diamond(cx, cy, tile_w, tile_h, rgb, alpha);
}

static int legal_target_kind(int from, int to)
{
    int kind = 0;
    for (int i = 0; i < G.legal_count; i++) {
        if (G.legal[i].from != from || G.legal[i].to != to) continue;
        kind |= G.legal[i].capture ? 2 : 1;
        if (G.legal[i].castle) kind |= 4;
    }
    return kind;
}

static void draw_board(void)
{
    for (int layer = 0; layer < 7; layer++) {
        float a = 0.025f * (7 - layer);
        fill_diamond(board_cx + shake_x, board_top + shake_y + tile_h * 4.55f + layer * 2.0f,
                     tile_w * 8.65f + layer * 5.0f,
                     tile_h * 8.45f + layer * 2.0f, 0x000000, a);
    }

    for (int vr = 0; vr < 8; vr++) {
        for (int f = 0; f < 8; f++) {
            int rank = 7 - vr;
            int sq = make_sq(f, rank);
            float cx, cy;
            square_center(sq, &cx, &cy);
            bool light = ((f + rank) & 1) == 0;
            uint32_t base = light ? 0xd8c1a6 : 0x55362f;
            uint32_t shade = light ? 0xf5e2bf : 0x2c1b22;
            fill_diamond(cx, cy, tile_w, tile_h, rgb_mix(base, shade, 0.20f), 0.62f);
            if (((f * 17 + rank * 31) & 7) == 0)
                fill_diamond(cx, cy, tile_w * 0.62f, tile_h * 0.62f,
                             light ? 0xffffff : 0x000000, light ? 0.035f : 0.045f);
        }
    }

    /* coordinates along the near edges */
    int near_rank = game_view_flipped() ? 7 : 0;
    int rank_edge_file = game_view_flipped() ? 7 : 0;
    for (int f = 0; f < 8; f++) {
        float cx, cy;
        square_center(make_sq(f, near_rank), &cx, &cy);
        char lab[2] = { (char)('a' + f), 0 };
        draw_text(cx + tile_w * 0.34f, cy + tile_h * 0.30f, lab, 0xe2d4bb, 0.55f, 1);
    }
    for (int r = 0; r < 8; r++) {
        float cx, cy;
        square_center(make_sq(rank_edge_file, r), &cx, &cy);
        char lab[2] = { (char)('1' + r), 0 };
        draw_text(cx - tile_w * 0.46f, cy + tile_h * 0.14f, lab, 0xe2d4bb, 0.55f, 1);
    }

    if (G.selected >= 0 && G.state == GS_PLAYING) {
        draw_square_overlay(G.selected, 0xfbbf24, 0.28f);
        float scx, scy;
        square_center(G.selected, &scx, &scy);
        outline_diamond(scx, scy, tile_w, tile_h, 0xfbbf24, 0.95f);
    }

    if (G.last_move[0]) {
        int f1 = G.last_move[0] - 'a', r1 = G.last_move[1] - '1';
        int f2 = G.last_move[2] - 'a', r2 = G.last_move[3] - '1';
        if (f1 >= 0 && f1 < 8 && r1 >= 0 && r1 < 8)
            draw_square_overlay(make_sq(f1, r1), 0xffffff, 0.08f);
        if (f2 >= 0 && f2 < 8 && r2 >= 0 && r2 < 8)
            draw_square_overlay(make_sq(f2, r2), 0xffffff, 0.12f);
    }

    /* Check remains visible until answered; the stronger pulse is brief. */
    if (G.state != GS_GAMEOVER && game_side_in_check(G.side)) {
        int king_sq = game_king_square(G.side);
        if (king_sq < 0) return;
        float cx, cy;
        square_center(king_sq, &cx, &cy);
        float pulse = G.check_flash > 0
            ? 0.55f + 0.45f * sinf(G.frame_count * 0.55f) : 0.45f;
        draw_square_overlay(king_sq, 0xef4444, 0.18f + 0.20f * pulse);
        outline_diamond(cx, cy, tile_w * 0.90f, tile_h * 0.90f,
                        0xfca5a5, 0.95f);
    }
}

static void draw_legal_markers(void)
{
    if (G.selected < 0 || G.state != GS_PLAYING) return;
    for (int sq = 0; sq < 64; sq++) {
        int kind = legal_target_kind(G.selected, sq);
        if (!kind) continue;
        float cx, cy;
        square_center(sq, &cx, &cy);
        if (kind & 2) {
            /* Capture targets use an outline, readable without relying on
             * the same color cue as quiet moves.  Markers are drawn over the
             * army sprites so tall foreground pieces cannot hide them. */
            outline_diamond(cx, cy, tile_w * 0.92f, tile_h * 0.92f,
                            0x180405, 1.0f);
            outline_diamond(cx, cy, tile_w * 0.84f, tile_h * 0.84f,
                            0xfb7185, 1.0f);
        } else {
            uint32_t color = kind & 4 ? 0x60a5fa : 0x4ade80;
            fill_circle(cx, cy, fmaxf(5.0f, tile_h * 0.16f), 0x07120b, 0.92f);
            fill_circle(cx, cy, fmaxf(3.0f, tile_h * 0.095f), color, 1.0f);
        }
    }
}

static bool skip_board_piece(int sq)
{
    if (G.state == GS_GAMEOVER && sq == G.loser_king_sq &&
        (G.result_kind == RESULT_CHECKMATE || G.result_kind == RESULT_RESIGN))
        return true;   /* replaced by the fallen king */
    if (!G.anim.active) return false;
    Move m = G.anim.move;
    if (sq == m.from) return true;
    if (m.capture && sq == G.anim.victim_sq) return true;
    if (G.anim.rook_from >= 0 && sq == G.anim.rook_from) return true;
    return false;
}

/* ---------- battle choreography ----------
 * capture timeline (t 0..1):
 *   0.00-0.34 approach march
 *   0.34-0.46 windup        0.46-0.60 strike (impact at ~0.5)
 *   0.55-0.85 victim topples and fades
 *   0.60-0.78 recover       0.78-1.00 claim the square
 */

typedef struct {
    bool has_effect;
    float fx, fy, fsize, falpha;
    int effect;
    bool has_spark;
    float px, py, pr, pa;
    uint32_t spark_col;
    bool has_trail;
    float ax, ay, tx, ty;
    int trail_dir;
    uint32_t trail_col;
    float trail_a;
    bool has_flash;             /* white hit-flash over the victim */
    float flx, fly, flr, fla;
} BattleFx;

static BattleFx battle_fx;

static void compute_capture_anim(void)
{
    Move m = G.anim.move;
    float sx, sy, tx, ty, vx, vy;
    square_center(m.from, &sx, &sy);
    square_center(m.to, &tx, &ty);
    square_center(G.anim.victim_sq, &vx, &vy);
    sy += tile_h * 0.33f;
    ty += tile_h * 0.33f;
    vy += tile_h * 0.33f;
    float t = clampf(G.anim.t, 0, 1);
    float size = tile_w * 1.02f;

    float dx = vx - sx;
    float dy = vy - sy;
    float len = sqrtf(dx * dx + dy * dy);
    float nx = len > 0.001f ? dx / len : 1.0f;
    float ny = len > 0.001f ? dy / len : 0.0f;
    /* a screen-vertical duel would stack both sprites on one column; give
     * the attacker a sideways stance so the exchange stays readable */
    if (fabsf(nx) < 0.30f) {
        float bias = sq_file(G.anim.victim_sq) < 4 ? 0.55f : -0.55f;
        nx = bias;
        ny = ny >= 0 ? 0.84f : -0.84f;
    }
    int dir = nx >= 0 ? 1 : -1;
    uint32_t side_col = piece_color(G.anim.attacker) == SIDE_WHITE ? 0xff6b4a : 0x38d9ff;
    uint32_t spark_col = G.anim.victim_type == PT_ROOK ? 0xd1d5db : 0xfacc15;

    float approach = ease_smooth(clampf(t / 0.34f, 0, 1));
    float windup = pulse_window(t, 0.38f, 0.52f);
    float strike = pulse_window(t, 0.47f, 0.64f);
    float impact = pulse_window(t, 0.51f, 0.80f);
    float fall = ease_smooth(clampf((t - 0.56f) / 0.30f, 0, 1));
    float claim = ease_smooth(clampf((t - 0.78f) / 0.20f, 0, 1));
    float fade = t > 0.86f ? 1.0f - clampf((t - 0.86f) / 0.12f, 0, 1) : 1.0f;

    float contact_x = vx - nx * tile_w * 0.60f;
    float contact_y = vy - ny * tile_h * 0.46f;
    float ax = sx + (contact_x - sx) * approach;
    float ay = sy + (contact_y - sy) * approach;
    ax -= nx * tile_w * 0.10f * windup;    /* rear back */
    ay -= ny * tile_h * 0.12f * windup;
    ax += nx * tile_w * 0.17f * strike;    /* lunge */
    ay += ny * tile_h * 0.22f * strike;
    ax = ax + (tx - ax) * claim;
    ay = ay + (ty - ay) * claim;

    /* whole-scene impact shake */
    float sh = impact > 0.55f ? (impact - 0.55f) * tile_w * 0.10f : 0.0f;
    shake_x = sh * sinf(G.frame_count * 2.3f);
    shake_y = sh * 0.5f * sinf(G.frame_count * 3.1f);

    float victim_shake = windup * sinf(G.frame_count * 1.1f) * tile_w * 0.02f
                       + impact * sinf(G.frame_count * 1.7f) * tile_w * 0.04f;
    float victim_x = vx + nx * tile_w * (0.13f * impact + 0.46f * fall) + victim_shake;
    float victim_y = vy + ny * tile_h * (0.18f * impact + 0.22f * fall)
                     + tile_h * 0.34f * fall;
    float victim_angle = dir * fall * 1.42f;

    /* attacker sprite; ghosts occupy the lower indices so the painter's
     * sweep lays the motion smear underneath the attacker itself */
    AnimSprite *att = &anim_sprites[2];
    att->used = true;
    att->piece = G.anim.attacker;
    att->size = size * (1.0f - 0.05f * windup + 0.09f * strike);
    att->alpha = 1.0f;
    att->angle = dir * (-0.14f * windup + 0.18f * strike);
    if (t < 0.34f) {
        float phase = clampf(t / 0.34f, 0, 1) * 2.0f;   /* two strides in */
        float footfall = fabsf(sinf(phase * 3.14159265f));
        att->x = ax;
        att->y = ay - tile_h * 0.07f * footfall;
        att->angle = -dir * 0.030f * sinf(phase * 6.2831853f);
        set_march_pose(att, G.anim.attacker, vy - sy);
    } else {
        att->x = ax;
        /* crouch into the windup, hop through the blow */
        att->y = ay + tile_h * 0.06f * windup - tile_h * 0.07f * strike;
        int pose = t < 0.41f ? 0 : t < 0.50f ? 1 : t < 0.62f ? 2 : t < 0.78f ? 3 : -1;
        if (pose < 0)
            set_static_pose(att, G.anim.attacker);
        else
            set_fight_pose(att, G.anim.attacker, pose, dir);
    }
    /* promotion flourish: the pawn is crowned as it claims the square */
    if (m.promo && t > 0.86f) {
        char promoted = piece_color(G.anim.attacker) == SIDE_WHITE
            ? (char)piece_upper(m.promo)
            : m.promo;
        att->piece = promoted;
        set_static_pose(att, promoted);
    }
    att->depth = depth_from_screen_y(att->y);

    /* motion-smear ghosts trail the lunge */
    if (strike > 0.15f && t < 0.70f) {
        for (int k = 1; k <= 2; k++) {
            AnimSprite *gh = &anim_sprites[k - 1];
            *gh = *att;
            gh->x -= nx * tile_w * 0.15f * k * strike;
            gh->y -= ny * tile_h * 0.15f * k * strike;
            gh->alpha = (k == 1 ? 0.30f : 0.15f) * strike;
            gh->depth = att->depth - 0.01f * k;
        }
    }

    /* victim sprite: en garde in its own colors, then knocked over */
    if (G.anim.victim != '.' && t < 0.97f) {
        AnimSprite *vic = &anim_sprites[3];
        vic->used = true;
        vic->piece = G.anim.victim;
        vic->x = victim_x;
        vic->y = victim_y;
        vic->size = size * (1.0f + impact * 0.05f);
        vic->alpha = fade;
        vic->angle = victim_angle;
        set_fight_pose(vic, G.anim.victim, 0, -dir);
        vic->depth = depth_from_screen_y(vy) + (fall > 0.28f ? 0.6f : -0.01f);
    }

    /* overlay effects */
    memset(&battle_fx, 0, sizeof battle_fx);
    if (strike > 0.06f) {
        battle_fx.has_trail = true;
        battle_fx.ax = ax;
        battle_fx.ay = ay;
        battle_fx.tx = vx;
        battle_fx.ty = vy;
        battle_fx.trail_dir = dir;
        battle_fx.trail_col = side_col;
        battle_fx.trail_a = clampf(strike * 1.15f, 0, 1);
    }
    if (impact > 0.02f) {
        float hit_x = vx - nx * tile_w * 0.08f;
        float hit_y = vy - tile_w * 0.70f - ny * tile_h * 0.16f;
        battle_fx.has_flash = true;
        battle_fx.flx = victim_x;
        battle_fx.fly = victim_y - size * 0.55f;
        battle_fx.flr = tile_w * 0.30f;
        battle_fx.fla = impact * 0.28f;
        battle_fx.has_effect = true;
        battle_fx.fx = hit_x;
        battle_fx.fy = hit_y;
        battle_fx.fsize = tile_w * (0.98f + impact * 0.34f);
        battle_fx.falpha = impact;
        battle_fx.effect = G.anim.effect;
        battle_fx.has_spark = true;
        battle_fx.px = hit_x;
        battle_fx.py = hit_y;
        battle_fx.pr = tile_w * (0.26f + impact * 0.18f);
        battle_fx.pa = impact;
        battle_fx.spark_col = spark_col;
    }
    (void)ty;
}

static void compute_march_anim(void)
{
    Move m = G.anim.move;
    float sx, sy, tx, ty;
    square_center(m.from, &sx, &sy);
    square_center(m.to, &tx, &ty);
    sy += tile_h * 0.33f;
    ty += tile_h * 0.33f;
    float t = clampf(G.anim.t, 0, 1);
    float size = tile_w * 1.02f;

    float move_t = ease_smooth(t);
    float ax = sx + (tx - sx) * move_t;
    float ay = sy + (ty - sy) * move_t;

    /* knights leap in an arc over anything in the way */
    if (G.anim.attacker_type == PT_KNIGHT)
        ay -= sinf(clampf(t, 0, 1) * 3.14159265f) * tile_h * 1.35f;

    int dist = abs(sq_file(m.to) - sq_file(m.from));
    int drank = abs(sq_rank(m.to) - sq_rank(m.from));
    if (drank > dist) dist = drank;
    float steps = fmaxf(2.0f, (float)dist * 1.5f);
    float phase = t * steps;                     /* one unit per footstep */
    float footfall = fabsf(sinf(phase * 3.14159265f));
    float settle = ease_smooth(clampf((t - 0.84f) / 0.16f, 0, 1));
    int dirx = tx >= sx ? 1 : -1;

    float bob = -tile_h * 0.075f * footfall * (1.0f - settle);
    float lean = -dirx * (0.030f + 0.022f * sinf(phase * 6.2831853f)) * (1.0f - settle);
    float pulse = 1.0f + 0.025f * footfall * (1.0f - settle);
    if (G.anim.attacker_type == PT_ROOK) {
        /* towers glide rather than strut */
        bob *= 0.25f;
        lean *= 0.3f;
        pulse = 1.0f;
    } else if (G.anim.attacker_type == PT_KNIGHT) {
        /* airborne: no footfalls, just a lean into the leap */
        bob = 0;
        lean = -dirx * 0.10f * sinf(clampf(t, 0, 1) * 3.14159265f);
        pulse = 1.0f;
    }

    AnimSprite *att = &anim_sprites[0];
    att->used = true;
    att->piece = G.anim.attacker;
    att->x = ax;
    att->y = ay + bob;
    att->size = size * pulse;
    att->alpha = 1.0f;
    att->angle = lean;
    if (m.promo && t > 0.82f) {
        char promoted = piece_color(G.anim.attacker) == SIDE_WHITE
            ? (char)piece_upper(m.promo)
            : m.promo;
        att->piece = promoted;
        set_static_pose(att, promoted);
    } else if (settle > 0.5f) {
        /* velocity is ~zero here, so the pose change reads as the piece
         * turning to take its place in the ranks */
        set_static_pose(att, G.anim.attacker);
    } else {
        set_march_pose(att, G.anim.attacker, ty - sy);
    }
    att->depth = depth_from_screen_y(ay);

    /* castling: the rook slides home during the second half */
    if (G.anim.rook_from >= 0) {
        float rsx, rsy, rtx, rty;
        square_center(G.anim.rook_from, &rsx, &rsy);
        square_center(G.anim.rook_to, &rtx, &rty);
        rsy += tile_h * 0.33f;
        rty += tile_h * 0.33f;
        float rt = ease_smooth(clampf((t - 0.45f) / 0.5f, 0, 1));
        AnimSprite *rook = &anim_sprites[2];
        rook->used = true;
        rook->piece = piece_color(G.anim.attacker) == SIDE_WHITE ? 'R' : 'r';
        rook->x = rsx + (rtx - rsx) * rt;
        rook->y = rsy + (rty - rsy) * rt
                  + sinf(clampf((t - 0.45f) / 0.5f, 0, 1) * 3.14159265f) * -tile_h * 0.12f;
        rook->size = size;
        rook->alpha = 1.0f;
        rook->angle = 0;
        rook->atlas = 0;
        rook->away = piece_color(rook->piece) ==
                     (game_view_flipped() ? SIDE_BLACK : SIDE_WHITE);
        rook->flip = motion_flip_for_piece(rook->piece, rook->away);
        rook->depth = depth_from_screen_y(rook->y);
    }

    /* promotion sparkle */
    memset(&battle_fx, 0, sizeof battle_fx);
    if (m.promo && t > 0.78f) {
        float glow = pulse_window(t, 0.78f, 1.0f);
        battle_fx.has_spark = true;
        battle_fx.px = tx;
        battle_fx.py = ty - tile_w * 0.55f;
        battle_fx.pr = tile_w * (0.35f + glow * 0.25f);
        battle_fx.pa = glow;
        battle_fx.spark_col = 0xfde047;
    }
}

static void compute_animation(void)
{
    memset(anim_sprites, 0, sizeof anim_sprites);
    memset(&battle_fx, 0, sizeof battle_fx);
    shake_x = shake_y = 0;
    if (!G.anim.active) {
        /* fallen king on the gameover board */
        if (G.state == GS_GAMEOVER && G.loser_king_sq >= 0 &&
            (G.result_kind == RESULT_CHECKMATE || G.result_kind == RESULT_RESIGN)) {
            float cx, cy;
            square_center(G.loser_king_sq, &cx, &cy);
            cy += tile_h * 0.33f;
            float fall = ease_smooth(clampf(G.over_t / 0.9f, 0, 1));
            AnimSprite *king = &anim_sprites[0];
            king->used = true;
            king->piece = G.winner_side == SIDE_WHITE ? 'k' : 'K';
            king->x = cx + fall * tile_w * 0.30f;
            king->y = cy + fall * tile_h * 0.20f;
            king->size = tile_w * 1.02f;
            king->alpha = 1.0f;
            king->angle = fall * 1.48f;
            king->atlas = 0;
            king->away = false;
            king->flip = false;
            king->depth = depth_from_screen_y(cy);
        }
        return;
    }
    if (G.anim.move.capture && G.anim.victim != '.')
        compute_capture_anim();
    else
        compute_march_anim();
}

/* painter's sweep: static pieces and animated sprites interleaved by depth */
static void draw_scene_pieces(void)
{
    float size = tile_w * 0.98f;
    bool drawn[MAX_ANIM_SPRITES] = { false };

    bool flipped = game_view_flipped();
    for (int diag = 0; diag <= 14; diag++) {
        for (int i = 0; i < MAX_ANIM_SPRITES; i++) {
            if (anim_sprites[i].used && !drawn[i] &&
                anim_sprites[i].depth < (float)diag) {
                draw_anim_sprite(&anim_sprites[i]);
                drawn[i] = true;
            }
        }
        for (int vr = 0; vr < 8; vr++) {
            int vf = diag - vr;
            if (vf < 0 || vf >= 8) continue;
            int f = flipped ? 7 - vf : vf;
            int rank = flipped ? vr : 7 - vr;
            int sq = make_sq(f, rank);
            char p = G.board[sq];
            if (p == '.' || skip_board_piece(sq)) continue;
            float cx, cy;
            square_center(sq, &cx, &cy);
            draw_piece_static(p, cx, cy + tile_h * 0.33f, size, 1.0f);
        }
    }
    for (int i = 0; i < MAX_ANIM_SPRITES; i++)
        if (anim_sprites[i].used && !drawn[i])
            draw_anim_sprite(&anim_sprites[i]);
}

static void draw_battle_fx(void)
{
    if (battle_fx.has_flash)
        fill_circle(battle_fx.flx, battle_fx.fly, battle_fx.flr,
                    0xffffff, battle_fx.fla);
    if (battle_fx.has_trail)
        draw_piece_attack_trail(G.anim.attacker_type,
                                battle_fx.ax, battle_fx.ay,
                                battle_fx.tx, battle_fx.ty,
                                battle_fx.trail_dir, battle_fx.trail_col,
                                battle_fx.trail_a);
    if (battle_fx.has_effect)
        draw_effect(battle_fx.effect, battle_fx.fx, battle_fx.fy,
                    battle_fx.fsize, battle_fx.falpha);
    if (battle_fx.has_spark)
        draw_spark_burst(battle_fx.px, battle_fx.py, battle_fx.pr,
                         battle_fx.spark_col, battle_fx.pa);
}

/* ---------- cursor ---------- */

static void draw_board_cursor(void)
{
    float cx, cy;
    square_center(G.cursor, &cx, &cy);
    uint32_t side_col = G.side == SIDE_WHITE ? 0xff4d3d : 0x38d9ff;
    float pulse = 0.72f + 0.28f * sinf(G.frame_count * 0.16f);

    outline_diamond(cx, cy, tile_w * 1.05f, tile_h * 1.05f, 0x050505, 1.0f);
    outline_diamond(cx, cy, tile_w * 0.96f, tile_h * 0.96f, side_col, 0.95f);
    fill_diamond(cx, cy, tile_w * 0.62f, tile_h * 0.62f, side_col, 0.12f * pulse);

    float px = cx + tile_w * 0.28f;
    float py = cy - tile_h * 1.34f;
    fill_triangle(px - 2, py - 2,
                  px + tile_w * 0.34f + 2, py + tile_h * 0.30f,
                  px + tile_w * 0.05f, py + tile_h * 0.43f + 2,
                  0x020202, 0.95f);
    fill_triangle(px, py,
                  px + tile_w * 0.34f, py + tile_h * 0.30f,
                  px + tile_w * 0.05f, py + tile_h * 0.43f,
                  0xfff7ed, 1.0f);
    draw_line(px + tile_w * 0.09f, py + tile_h * 0.39f,
              px + tile_w * 0.22f, py + tile_h * 0.70f,
              4.0f, 0x020202, 0.95f);
    draw_line(px + tile_w * 0.09f, py + tile_h * 0.39f,
              px + tile_w * 0.22f, py + tile_h * 0.70f,
              2.0f, side_col, 1.0f);
}

/* ---------- HUD ---------- */

static void draw_captured_tray(int side, float x, float y, bool align_right,
                               float max_span)
{
    int n = G.captured_count[side];
    if (n <= 0) return;
    float cell = 22.0f;
    if (cell * n > max_span)
        cell = max_span / n;   /* squeeze rather than spill over the HUD */
    for (int i = 0; i < n; i++) {
        char p = G.captured[side][i];
        int type = piece_type(p);
        int col = piece_color(p);
        if (type < 0 || col < 0) continue;
        float cx = align_right ? x - i * cell : x + i * cell;
        draw_sprite_cell_pose(&piece_atlas, 6, 2, type, col,
                              cx, y, 30.0f, 0.95f, false, 0);
    }
}

static void hud_move_list(char *buf, size_t len)
{
    /* last few moves off the end of the UCI history */
    const char *h = G.uci_history;
    size_t hl = strlen(h);
    const char *p = h + hl;
    int words = 0;
    while (p > h && words < 4) {
        p--;
        if (*p == ' ') words++;
    }
    if (p > h) p++;
    snprintf(buf, len, "%s", p);
}

static void draw_hud(void)
{
    int top_h = hud_top_height();
    fill_rect(0, 0, W, top_h, 0x080a12, 0.88f);
    fill_rect(0, H - 48, W, 48, 0x080a12, 0.88f);
    char buf[256];
    int main_y = top_h >= 60 ? 12 : 3;
    int status_y = top_h >= 60 ? 36 : 24;

    int hud_side = G.anim.active ? piece_color(G.anim.attacker) : G.side;
    uint32_t side_col = hud_side == SIDE_WHITE ? 0xffb4a2 : 0xa5f3fc;
    if (W < 720) {
        const char *compact_mode = G.mode == MODE_HUMAN_HUMAN ? "2 Players"
                                 : G.mode == MODE_HUMAN_AI ? "vs Computer"
                                 : "Computer Match";
        snprintf(buf, sizeof buf, "%s", compact_mode);
    } else {
        snprintf(buf, sizeof buf, "%s", mode_name(G.mode));
    }
    draw_text(18, main_y, buf, 0xf8fafc, 1, 1);

    if (G.state == GS_GAMEOVER) {
        snprintf(buf, sizeof buf, "the battle is over");
        side_col = 0xcbd5e1;
    } else if (G.ai_thinking) {
        int dots = (G.frame_count / 18) % 4;
        snprintf(buf, sizeof buf, "%s thinking%.*s", side_name(hud_side), dots, "...");
    } else if (!G.anim.active && game_side_in_check(G.side)) {
        snprintf(buf, sizeof buf, "%s - CHECK", side_name(G.side));
        side_col = 0xfca5a5;
    } else {
        snprintf(buf, sizeof buf, "%s %s", side_name(hud_side),
                 G.anim.active ? "acting" : "to move");
    }
    draw_text_center(W * 0.5f, main_y, buf, side_col, 1, 1);

    char mvs[64];
    hud_move_list(mvs, sizeof mvs);
    if (mvs[0] && W >= 720) {
        snprintf(buf, sizeof buf, "move %d  %s", G.fullmove_number, mvs);
        int tw = text_width(buf, 1);
        draw_text(W - tw - 18, main_y, buf, 0x94a3b8, 1, 1);
    }

    snprintf(buf, sizeof buf, "%s", G.status);
    if (top_h >= 60)
        draw_text(18, 36, buf, game_side_in_check(G.side) ? 0xfacc15 : 0xa1a1aa, 1, 1);
    else
        draw_text_center(W * 0.5f, status_y, buf,
                         game_side_in_check(G.side) ? 0xfacc15 : 0xcbd5e1, 1, 1);
    /* captured trays float in the empty corners under the top bar */
    draw_captured_tray(SIDE_WHITE, 26.0f, top_h + 40.0f, false, W * 0.30f);
    draw_captured_tray(SIDE_BLACK, W - 26.0f, top_h + 40.0f, true, W * 0.30f);

    const char *help;
    if (G.state == GS_GAMEOVER) {
        help = "ENTER menu   R rematch   Q quit";
    } else if (G.anim.active) {
        help = W >= 900
            ? "S / ENTER skip battle   F fast battles   M music   Q quit"
            : "S/ENTER skip  F fast  M music  Q quit";
    } else if (G.selected >= 0) {
        help = W >= 900
            ? "ARROWS/WASD target   ENTER move   ESC cancel   R resign   N leave   Q quit"
            : "WASD target  ENTER move  ESC cancel  N leave";
    } else if (G.state == GS_PLAYING && !game_is_human_turn()) {
        help = "Computer thinking   R resign   N leave   M music   Q quit";
    } else {
        help = W >= 900
            ? "ARROWS/WASD move   ENTER select   R resign   M music   F fast battles   N leave   Q quit"
            : "WASD move  ENTER select  R resign  N leave  Q quit";
    }
    if (text_width(help, 1) > W - 24)
        help = "WASD ENTER R M N Q";
    draw_text(16, H - 31, help, 0x94a3b8, 1, 1);

    if (G.anim.active && G.anim.caption[0]) {
        int scale = G.anim.move.capture ? 2 : 1;
        int tw = text_width(G.anim.caption, scale) + 36;
        int x = (W - tw) / 2;
        int y = H - (scale == 2 ? 112 : 92);
        fill_rect(x, y, tw, scale == 2 ? 44 : 32, 0x12080a, 0.82f);
        draw_text_center(W / 2.0f, y + (scale == 2 ? 10 : 8), G.anim.caption,
                         piece_color(G.anim.attacker) == SIDE_WHITE ? 0xffd0a2 : 0xa5f3fc,
                         1, scale);
    }

    if (G.check_flash > 0.4f && G.state == GS_PLAYING) {
        float a = clampf((G.check_flash - 0.4f) / 0.6f, 0, 1);
        draw_text_banner(W * 0.5f, top_h + 16, "CHECK!", 0xef4444, a, 2);
    }
}

/* ---------- promotion chooser ---------- */

static void draw_promotion_overlay(void)
{
    static const int promo_types[4] = { PT_QUEEN, PT_ROOK, PT_BISHOP, PT_KNIGHT };
    static const char *promo_names[4] = { "Queen", "Rook", "Bishop", "Knight" };

    fill_rect(0, 0, W, H, 0x02040a, 0.45f);
    int pw = 420, ph = 190;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    fill_rect(x, y, pw, ph, 0x0b0e18, 0.94f);
    fill_rect(x, y, pw, 3, 0xfacc15, 0.9f);
    draw_text_center(W * 0.5f, y + 14, "A pawn earns its crown", 0xf8fafc, 1, 1);

    float cell = pw / 4.0f;
    int side = G.side;
    for (int i = 0; i < 4; i++) {
        float cx = x + cell * (i + 0.5f);
        float cy = y + 128;
        if (i == G.promo_cursor) {
            fill_rect((int)(cx - cell * 0.42f), y + 40, (int)(cell * 0.84f), 116,
                      0x334155, 0.6f);
            fill_circle(cx, cy + 6, cell * 0.34f, 0xfacc15, 0.18f);
        }
        char piece = "QRBN"[i];
        if (side == SIDE_BLACK) piece = (char)(piece + 32);
        bool away = side == (game_view_flipped() ? SIDE_BLACK : SIDE_WHITE);
        draw_piece_direction_pose(piece, cx, cy, cell * 0.72f, 1.0f,
                                  away, motion_flip_for_piece(piece, away), 0);
        draw_text_center(cx, y + 148, promo_names[i],
                         i == G.promo_cursor ? 0xfacc15 : 0xcbd5e1, 1, 1);
        (void)promo_types;
    }
    draw_text_center(W * 0.5f, y + ph - 20, "LEFT/RIGHT choose   ENTER confirm   ESC cancel",
                     0x94a3b8, 1, 1);
}

/* ---------- gameover ---------- */

static void draw_fireworks(void)
{
    if (G.winner_side < 0) return;
    uint32_t cols[3] = { 0xfacc15, G.winner_side == SIDE_WHITE ? 0xff6b4a : 0x38d9ff, 0xf8fafc };
    for (int burst = 0; burst < 3; burst++) {
        float bt = fmodf(G.over_t * 0.8f + burst * 0.37f, 1.1f);
        if (bt > 1.0f) continue;
        float bx = W * (0.22f + 0.56f * ((burst * 47 + (int)(G.over_t / 1.4f) * 31) % 100) / 100.0f);
        float by = H * (0.18f + 0.24f * ((burst * 83 + (int)(G.over_t / 1.4f) * 17) % 100) / 100.0f);
        float r = bt * W * 0.06f;
        float a = (1.0f - bt) * 0.9f;
        for (int i = 0; i < 12; i++) {
            float ang = i * 0.5235988f;
            fill_circle(bx + cosf(ang) * r, by + sinf(ang) * r * 0.7f,
                        2.5f + (1.0f - bt) * 2.0f, cols[burst % 3], a);
        }
    }
}

static void draw_gameover_overlay(void)
{
    float slide = ease_smooth(clampf(G.over_t / 0.7f, 0, 1));
    if (slide <= 0.01f) return;

    draw_fireworks();

    int pw = 560, ph = 150;
    if (pw > W - 40) pw = W - 40;
    int x = (W - pw) / 2;
    int y = (int)((H / 2 - 75) * slide + (float)H * (1.0f - slide));
    uint32_t band = G.winner_side < 0 ? 0x64748b
                  : G.winner_side == SIDE_WHITE ? 0xff4d3d : 0x38d9ff;
    fill_rect(x, y, pw, ph, 0x09090b, 0.92f);
    fill_rect(x, y, pw, 4, band, 0.95f);
    fill_rect(x, y + ph - 4, pw, 4, band, 0.95f);

    int scale = text_width(G.result[0] ? G.result : "Game over", 2) < pw - 30 ? 2 : 1;
    draw_text_banner(W / 2.0f, y + 30, G.result[0] ? G.result : "Game over",
                     0xf8fafc, 1, scale);
    draw_text_center(W / 2.0f, y + 78, "ENTER menu    R rematch    Q quit", 0x94a3b8, 1, 1);
    if (G.winner_side >= 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "the %s army is victorious", side_name(G.winner_side));
        draw_text_center(W / 2.0f, y + 108, buf,
                         G.winner_side == SIDE_WHITE ? 0xffd0a2 : 0xa5f3fc, 1, 1);
    }
}

/* ---------- intro ---------- */

static void draw_intro(void)
{
    float t = G.intro_t;
    clear_bg(0x000000, 0x02040a);

    /* backdrop fades in with a slow zoom */
    float img_a = ease_smooth(clampf((t - 0.4f) / 1.2f, 0, 1));
    if (img_a > 0.01f && title_img.ok) {
        draw_bitmap_cover_zoom(&title_img, 1.0f + 0.05f * t);
        fill_rect(0, 0, W, H, 0x000000, 1.0f - img_a * 0.75f);
    }

    /* title slams down with a little overshoot */
    if (t > 1.1f) {
        float tt = clampf((t - 1.1f) / 0.55f, 0, 1);
        float over = tt < 0.8f ? tt / 0.8f : 1.0f + 0.18f * sinf((tt - 0.8f) / 0.2f * 3.14159f);
        float ty = H * 0.30f * over - 80.0f * (1.0f - over);
        int scale = W >= 820 ? 5 : 3;
        draw_text_banner(W * 0.5f, ty, "CHESS BASH", 0xfacc15, ease_smooth(tt), scale);
    }
    if (t > 1.9f) {
        float a = ease_smooth(clampf((t - 1.9f) / 0.7f, 0, 1));
        draw_text_banner(W * 0.5f, H * 0.30f + (W >= 820 ? 96 : 64),
                         "the armies take the field", 0x93c5fd, a, 1);
    }
    if (t > 2.8f) {
        float pulse = 0.55f + 0.45f * sinf(t * 4.0f);
        draw_text_center(W * 0.5f, H * 0.78f, "press any key",
                         0xf8fafc, pulse, W >= 820 ? 2 : 1);
    }
    /* letterbox bars for a cinematic open */
    float bar = (1.0f - ease_smooth(clampf(t / 1.6f, 0, 1))) * H * 0.5f
              + ease_smooth(clampf(t / 1.6f, 0, 1)) * H * 0.06f;
    fill_rect(0, 0, W, (int)bar, 0x000000, 0.92f);
    fill_rect(0, H - (int)bar, W, (int)bar, 0x000000, 0.92f);
}

/* ---------- title menu ---------- */

static void title_attract_battle(float x, float y, float size)
{
    /* two pawns endlessly trading blows under the menu */
    float cyc = fmodf(G.title_t, 3.2f);
    int red_pose = cyc < 0.9f ? 0 : cyc < 1.25f ? 1 : cyc < 1.6f ? 2 : 0;
    int blue_pose = cyc < 1.7f ? 0 : cyc < 2.5f ? 0 : cyc < 2.85f ? 1 : 2;
    if (!fight_atlas.ok) return;
    draw_sprite_cell_pose(&fight_atlas, 6, 8, PT_PAWN, red_pose,
                          x - size * 0.62f, y, size,
                          0.95f, cell_flip(fight_facing, red_pose, PT_PAWN, 1, false), 0);
    draw_sprite_cell_pose(&fight_atlas, 6, 8, PT_PAWN, 4 + blue_pose,
                          x + size * 0.62f, y, size,
                          0.95f, cell_flip(fight_facing, 4 + blue_pose, PT_PAWN, -1, false), 0);
    if ((red_pose == 2 && cyc > 1.3f && cyc < 1.55f) ||
        (blue_pose == 2 && cyc > 2.9f && cyc < 3.15f))
        draw_spark_burst(x, y - size * 0.5f, size * 0.3f, 0xfacc15, 0.8f);
}

static void draw_title(void)
{
    float zoom = 1.02f + 0.045f * sinf(G.title_t * 0.11f);
    draw_bitmap_cover_zoom(&title_img, zoom);
    fill_rect(0, 0, W, H, 0x02040a, 0.42f);
    fill_rect(0, 0, W, 92, 0x02040a, 0.48f);
    draw_text_banner(W / 2.0f, 20, "CHESS BASH", 0xfacc15, 1, W >= 820 ? 4 : 3);
    draw_text_center(W / 2.0f, W >= 820 ? 84 : 72,
                     "animated terminal chess for Kitty / kilix", 0x93c5fd, 1, 1);

    int pw = 560, ph = 320;
    if (pw > W - 30) pw = W - 30;
    if (ph > H - 160) ph = H - 160;
    int px = (W - pw) / 2;
    int py = H - ph - 40;
    if (py < 110) py = 110;
    fill_rect(px, py, pw, ph, 0x070a12, 0.88f);
    fill_rect(px, py, pw, 3, 0xfacc15, 0.75f);

    char val[80];
    int row_h = (ph - 66) / MENU_COUNT;
    if (row_h > 38) row_h = 38;
    for (int i = 0; i < MENU_COUNT; i++) {
        int ry = py + 22 + i * row_h;
        bool enabled = !(i == MENU_SIDE && G.mode != MODE_HUMAN_AI) &&
                       !(i == MENU_DIFF && G.mode == MODE_HUMAN_HUMAN);
        bool sel = i == G.title_row;
        uint32_t col = !enabled ? 0x475569 : sel ? 0xfacc15 : 0xe5e7eb;
        if (sel)
            fill_rect(px + 14, ry - 5, pw - 28, row_h - 4, 0x334155, 0.62f);

        const char *label = "";
        val[0] = '\0';
        switch (i) {
        case MENU_MODE:
            label = "Mode";
            snprintf(val, sizeof val, "%s", mode_name(G.mode));
            break;
        case MENU_SIDE:
            label = "Play as";
            snprintf(val, sizeof val, "%s",
                     G.human_white ? "Red (first)" : "Blue (rotated)");
            break;
        case MENU_DIFF:
            label = "Difficulty";
            snprintf(val, sizeof val, "%s", difficulty_name(G.difficulty));
            break;
        case MENU_BOARD:
            label = "Battlefield";
            snprintf(val, sizeof val, "%s", board_theme_name(G.board_theme));
            break;
        case MENU_MUSIC:
            label = "Music";
            snprintf(val, sizeof val, "%s", G.music_on ? "On" : "Off");
            break;
        case MENU_SPEED:
            label = "Battles";
            snprintf(val, sizeof val, "%s", G.fast_anim ? "Fast" : "Full");
            break;
        case MENU_START:
            label = sel ? "> Begin battle <" : "Begin battle";
            break;
        }
        if (i == MENU_START) {
            draw_text_center(px + pw / 2.0f, ry, label, col, 1, 1);
        } else {
            draw_text(px + 34, ry, label, col, 1, 1);
            if (val[0]) {
                char shown[96];
                if (sel && enabled)
                    snprintf(shown, sizeof shown, "< %s >", val);
                else
                    snprintf(shown, sizeof shown, "%s", val);
                int tw = text_width(shown, 1);
                draw_text(px + pw - 34 - tw, ry, shown, col, 1, 1);
            }
        }
    }
    draw_text_center(px + pw / 2.0f, py + ph - 26,
                     W >= 700
                         ? "UP/DOWN choose   LEFT/RIGHT change   ENTER select   Q quit"
                         : "ARROWS choose/change   ENTER select   Q quit",
                     0x94a3b8, 1, 1);

    title_attract_battle(px + pw / 2.0f, (float)py - 12.0f, 64.0f);
}

/* ---------- top level ---------- */

void render_init(int w, int h)
{
    W = w;
    H = h;
    fb = malloc((size_t)W * H * 4);
    (void)sr_canvas_init(&canvas, W, H);
    title_img = load_ppm(asset_path("title.ppm"));
    piece_atlas = load_ppm(asset_path("pieces.ppm"));
    piece_direction_atlas = load_ppm(asset_path("pieces_direction.ppm"));
    fight_atlas = load_ppm(asset_path("pieces_fight.ppm"));
    effect_atlas = load_ppm(asset_path("effects.ppm"));
    render_set_theme(G.board_theme);
}

static bool expect_bitmap(const Bitmap *b, const char *name, int w, int h,
                          char *error, size_t error_len)
{
    if (b->ok && b->px && b->w == w && b->h == h) return true;
    if (error && error_len) {
        if (!b->ok || !b->px)
            snprintf(error, error_len, "required asset %s is missing or corrupt", name);
        else
            snprintf(error, error_len,
                     "required asset %s is %dx%d; expected %dx%d",
                     name, b->w, b->h, w, h);
    }
    return false;
}

bool render_validate_assets(char *error, size_t error_len)
{
    if (error && error_len) error[0] = '\0';
    if (!fb || !canvas.px) {
        if (error && error_len)
            snprintf(error, error_len, "render framebuffer allocation failed");
        return false;
    }
    if (!expect_bitmap(&title_img, "title.ppm", 640, 360, error, error_len) ||
        !expect_bitmap(&piece_atlas, "pieces.ppm", 768, 256, error, error_len) ||
        !expect_bitmap(&piece_direction_atlas, "pieces_direction.ppm", 768, 512,
                       error, error_len) ||
        !expect_bitmap(&fight_atlas, "pieces_fight.ppm", 768, 1024,
                       error, error_len) ||
        !expect_bitmap(&effect_atlas, "effects.ppm", 640, 384,
                       error, error_len))
        return false;

    for (int i = 0; i < BOARD_THEME_COUNT; i++) {
        char rel[128];
        snprintf(rel, sizeof rel, "boards/%s.ppm", themes[i].file);
        Bitmap theme = load_ppm(asset_path(rel));
        bool ok = expect_bitmap(&theme, rel, 640, 360, error, error_len);
        free_bitmap(&theme);
        if (!ok) return false;
    }
    return true;
}

void render_resize(int w, int h)
{
    if (w == W && h == H && fb && canvas.px) return;
    free(fb);
    sr_canvas_free(&canvas);
    W = w;
    H = h;
    fb = malloc((size_t)W * H * 4);
    (void)sr_canvas_init(&canvas, W, H);
}

void render_shutdown(void)
{
    free(fb);
    fb = NULL;
    sr_canvas_free(&canvas);
    free_bitmap(&title_img);
    free_bitmap(&board_bg_img);
    free_bitmap(&piece_atlas);
    free_bitmap(&piece_direction_atlas);
    free_bitmap(&fight_atlas);
    free_bitmap(&effect_atlas);
    loaded_theme = -1;
}

void render_frame(void)
{
    if (!fb || !canvas.px) return;
    if (G.state == GS_INTRO) {
        draw_intro();
        return;
    }
    if (G.state == GS_TITLE) {
        draw_title();
        return;
    }

    setup_board_metrics();
    compute_animation();

    if (board_bg_img.ok) {
        draw_bitmap_cover(&board_bg_img);
        fill_rect(0, 0, W, H, 0x02040a, 0.14f);
    } else {
        clear_bg(0x101827, 0x02040a);
        for (int i = 0; i < 160; i++) {
            int x = (i * 977 + 31) % W;
            int y = (i * 577 + 73) % H;
            uint32_t c = ((i + G.frame_count / 12) & 3) ? 0x334155 : 0x64748b;
            px_blend(x, y, c, 0.45f);
        }
    }
    draw_board();
    draw_scene_pieces();
    if (G.anim.active)
        draw_battle_fx();
    draw_legal_markers();
    if (G.state == GS_PLAYING && game_is_human_turn())
        draw_board_cursor();
    draw_hud();
    if (G.state == GS_PROMOTING)
        draw_promotion_overlay();
    if (G.state == GS_GAMEOVER)
        draw_gameover_overlay();
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&canvas, path);
}
