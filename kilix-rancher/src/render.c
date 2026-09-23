#include "kilix.h"
#include "soft_raster.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LW 960
#define LH 540

static sr_canvas canvas;
static sr_canvas presented;
static uint8_t *output;
static int output_width;
static int output_height;
static Bitmap ranch_background;
static Bitmap arena_background;
static Bitmap kilix_sprite;
static Bitmap kilix_atlas;              /* 6-frame animation strip (optional) */
static Bitmap opponent_sprites[OPPONENT_COUNT];
static Bitmap opponent_atlas[OPPONENT_COUNT];   /* 6-frame rival strips (optional) */

/* Frame order inside kilix_atlas.ppm (and every rival atlas). */
enum {
    KF_IDLE, KF_WALK, KF_NAP, KF_CROUCH, KF_POUNCE, KF_HURT, KILIX_FRAMES
};

/* Rival sprite/atlas files, indexed by opponent id (matches OPPONENTS[]). */
static const char *const OPPONENT_SPRITE_FILES[OPPONENT_COUNT] = {
    "opponents/mossnub.ppm",
    "opponents/dewdrop.ppm",
    "opponents/mistwing.ppm",
    "opponents/stonecalf.ppm",
    "opponents/moonmoth.ppm",
    "opponents/duskcub.ppm",
};
static const char *const OPPONENT_ATLAS_FILES[OPPONENT_COUNT] = {
    "opponents/mossnub_atlas.ppm",
    "opponents/dewdrop_atlas.ppm",
    "opponents/mistwing_atlas.ppm",
    "opponents/stonecalf_atlas.ppm",
    "opponents/moonmoth_atlas.ppm",
    "opponents/duskcub_atlas.ppm",
};

/* Care-basket item sprites (ITEMS[] order) and a journal-page backdrop; all
 * optional, with procedural fallbacks. */
static Bitmap care_sprites[ITEM_COUNT];
static Bitmap journal_background;
static Bitmap rent_sprite;       /* the monthly rent collector */
static const char *const CARE_SPRITE_FILES[ITEM_COUNT] = {
    "care/stew.ppm", "care/brush.ppm", "care/tonic.ppm", "care/treat.ppm",
};
/* Drill emblem icons, chosen per drill by its primary stat. */
static Bitmap drill_icons[STAT_COUNT];
static const char *const DRILL_ICON_FILES[STAT_COUNT] = {
    "icons/heart.ppm",    /* STAT_LIFE */
    "icons/claw.ppm",     /* STAT_POWER */
    "icons/flame.ppm",    /* STAT_INTELLECT */
    "icons/guard.ppm",    /* STAT_DEFENSE */
    "icons/agility.ppm",  /* STAT_SPEED */
    "icons/focus.ppm",    /* STAT_SKILL */
};
/* Mini-game prop sprites (all optional; draw_drill falls back to procedural). */
static Bitmap mg_board;          /* instruction signboard */
static Bitmap mg_num[3];         /* countdown digits 1, 2, 3 */
static Bitmap mg_flame;          /* mash gauge */
static Bitmap mg_bell;           /* rhythm beat */
static Bitmap mg_shelter;        /* hold marker */

static const uint32_t INK = 0x241B1A;
static const uint32_t CREAM = 0xFFF1C9;
static const uint32_t GOLD = 0xF6C453;
static const uint32_t ORANGE = 0xF26A2E;
static const uint32_t CORAL = 0xE94D3D;
static const uint32_t NIGHT = 0x182B3A;

const uint8_t *render_fb(void)
{
    return output;
}

static float smoothstep(float edge0, float edge1, float value)
{
    float t = clampf((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static uint32_t color_mix(uint32_t a, uint32_t b, float t)
{
    return sr_mix(a, b, t);
}

static void pixel_set(int x, int y, uint32_t color)
{
    sr_px(&canvas, x, y, color);
}

static void pixel_blend(int x, int y, uint32_t color, float alpha)
{
    sr_blend(&canvas, x, y, color, alpha);
}

static void clear_canvas(uint32_t color)
{
    sr_clear(&canvas, color);
}

static void fill_rect(int x, int y, int width, int height,
                      uint32_t color, float alpha)
{
    sr_fill_rect(&canvas, (float)x, (float)y, (float)width, (float)height,
                 color, alpha);
}

static void fill_gradient(int x, int y, int width, int height,
                          uint32_t top_color, uint32_t bottom_color,
                          float alpha)
{
    for (int py = y; py < y + height; py++) {
        float t = height > 1 ? (float)(py - y) / (height - 1) : 0.0f;
        uint32_t color = color_mix(top_color, bottom_color, t);
        for (int px = x; px < x + width; px++)
            pixel_blend(px, py, color, alpha);
    }
}

static void fill_circle(float cx, float cy, float radius,
                        uint32_t color, float alpha)
{
    sr_fill_circle(&canvas, cx, cy, radius, color, alpha);
}

static void fill_ellipse(float cx, float cy, float radius_x, float radius_y,
                         uint32_t color, float alpha)
{
    sr_fill_ellipse(&canvas, cx, cy, radius_x, radius_y, color, alpha);
}

static void fill_triangle(float ax, float ay, float bx, float by,
                          float cx, float cy, uint32_t color, float alpha)
{
    sr_fill_triangle(&canvas, ax, ay, bx, by, cx, cy, color, alpha);
}

static void draw_line(float x0, float y0, float x1, float y1, float width,
                      uint32_t color, float alpha)
{
    sr_line(&canvas, x0, y0, x1, y1, width, color, alpha, 0, 0);
}

static void rounded_rect(int x, int y, int width, int height, int radius,
                         uint32_t color, float alpha)
{
    if (width <= 0 || height <= 0) return;
    /* Keep the corner discs inside the rect: past this limit the "right"
     * corners sit left of the "left" corners and the shape paints wider than
     * `width` (a nearly-empty progress bar would render as a blob spilling
     * outside its track). */
    int limit = ((width < height ? width : height) - 1) / 2;
    if (radius > limit) radius = limit;
    if (radius < 1) {
        fill_rect(x, y, width, height, color, alpha);
        return;
    }
    fill_rect(x + radius, y, width - radius * 2, height, color, alpha);
    fill_rect(x, y + radius, width, height - radius * 2, color, alpha);
    fill_circle(x + radius, y + radius, radius, color, alpha);
    fill_circle(x + width - radius - 1, y + radius, radius, color, alpha);
    fill_circle(x + radius, y + height - radius - 1, radius, color, alpha);
    fill_circle(x + width - radius - 1, y + height - radius - 1,
                radius, color, alpha);
}

static void panel(int x, int y, int width, int height, uint32_t color,
                  float alpha)
{
    rounded_rect(x + 4, y + 5, width, height, 13, 0x000000, alpha * 0.45f);
    rounded_rect(x, y, width, height, 13, color, alpha);
    fill_rect(x + 12, y + 3, width - 24, 2, GOLD, alpha * 0.75f);
}

/* Compact original 5x7 uppercase font.  Lowercase text is folded to uppercase
 * at draw time, which gives the UI a readable late-console display style. */
static const char GLYPH_ORDER[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,!?-:+/()'&%#*<>=$[]";

static const uint8_t GLYPHS[][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {0,0,0,0,0,4,4}, {0,0,0,0,4,4,8},
    {4,0,4,4,4,0,4}, {14,17,1,2,4,0,4},
    {0,0,0,31,0,0,0}, {0,0,4,0,0,4,0},
    {0,4,4,31,4,4,0}, {1,2,4,8,16,0,0},
    {2,4,8,8,8,4,2}, {8,4,2,2,2,4,8},
    {4,4,0,0,0,0,0}, {8,20,8,21,18,13,0},
    {17,2,4,8,17,0,0}, {10,31,10,31,10,0,0},
    {4,21,14,31,14,21,4}, {1,2,4,8,4,2,1},
    {16,8,4,2,4,8,16}, {0,31,0,31,0,0,0},
    {4,15,20,14,5,30,4}, {14,8,8,8,8,8,14},
    {14,2,2,2,2,2,14}
};

static const uint8_t *glyph_for(char character)
{
    static const uint8_t blank[7] = {0};
    if (character >= 'a' && character <= 'z')
        character = (char)(character - 'a' + 'A');
    const char *found = strchr(GLYPH_ORDER, character);
    if (!found) return blank;
    size_t index = (size_t)(found - GLYPH_ORDER);
    if (index >= sizeof GLYPHS / sizeof GLYPHS[0]) return blank;
    return GLYPHS[index];
}

static int text_width(const char *text, int scale)
{
    if (!*text) return 0;
    return (int)strlen(text) * 6 * scale - scale;
}

/* Generated hand-lettered display font: a monospace grayscale glyph atlas
 * (assets/font.ppm). Each cell holds one glyph as luminance-on-magenta; the
 * text colour multiplies the luminance, so one atlas serves every UI colour.
 * Used for scale >= 2 where a detailed glyph reads well; the crisp 5x7 bitmap
 * stays for tiny (scale 1) text and any glyph the atlas lacks. */
static bool is_chroma(uint32_t color);   /* defined with the bitmap helpers */
#define FONT_CELL_W 40
#define FONT_CELL_H 56
static Bitmap font_atlas;
static const char FONT_ORDER[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,!?:'-/%+&;()";

static int font_index(char c)
{
    if (c == '\0') return -1;                        /* don't match the NUL */
    const char *f = strchr(FONT_ORDER, c);
    return f ? (int)(f - FONT_ORDER) : -1;
}

static void draw_glyph_cell(int cell, int x, int y, int w, int h,
                            uint32_t color, float alpha)
{
    int cells = font_atlas.w / FONT_CELL_W;
    if (cell < 0 || cell >= cells || w <= 0 || h <= 0) return;
    int base = cell * FONT_CELL_W;
    int tr = (color >> 16) & 255, tg = (color >> 8) & 255, tb = color & 255;
    int ah = font_atlas.h < FONT_CELL_H ? font_atlas.h : FONT_CELL_H;
    /* Area-average the 40x56 glyph cell down to the requested WxH: for each
     * destination pixel, average the luminance of the source block it covers
     * (magenta counts as empty), and use the covered fraction as the pixel's
     * alpha. This anti-aliases the down-scale so edges stay smooth instead of
     * the jagged fringe nearest-neighbour point-sampling produced. */
    for (int dy = 0; dy < h; dy++) {
        int sy0 = dy * ah / h, sy1 = (dy + 1) * ah / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < w; dx++) {
            int sx0 = base + dx * FONT_CELL_W / w;
            int sx1 = base + (dx + 1) * FONT_CELL_W / w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            long acc = 0;
            int covered = 0, area = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t p = font_atlas.px[sy * font_atlas.w + sx];
                    area++;
                    if (is_chroma(p)) continue;
                    acc += p & 255;                  /* grayscale: any channel */
                    covered++;
                }
            if (!covered) continue;
            int lum = (int)(acc / covered);
            float cov = (float)covered / (float)area;
            uint32_t c = (uint32_t)(tr * lum / 255) << 16 |
                         (uint32_t)(tg * lum / 255) << 8 |
                         (uint32_t)(tb * lum / 255);
            pixel_blend(x + dx, y + dy, c, alpha * cov);
        }
    }
}

static void draw_text(int x, int y, const char *text, uint32_t color,
                      float alpha, int scale)
{
    int origin = x;
    bool use_font = scale >= 2 && font_atlas.ok;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '\n') {
            y += 9 * scale;
            x = origin;
            continue;
        }
        int gi = use_font ? font_index(*cursor) : -1;
        if (gi >= 0) {
            draw_glyph_cell(gi, x, y, 5 * scale, 7 * scale, color, alpha);
        } else {
            const uint8_t *rows = glyph_for(*cursor);
            for (int gy = 0; gy < 7; gy++)
                for (int gx = 0; gx < 5; gx++)
                    if (rows[gy] & (1 << (4 - gx)))
                        fill_rect(x + gx * scale, y + gy * scale,
                                  scale, scale, color, alpha);
        }
        x += 6 * scale;
    }
}

static void draw_text_shadow(int x, int y, const char *text, uint32_t color,
                             float alpha, int scale)
{
    draw_text(x + scale, y + scale, text, 0x000000, alpha * 0.7f, scale);
    draw_text(x, y, text, color, alpha, scale);
}

static void draw_text_center(int center_x, int y, const char *text,
                             uint32_t color, float alpha, int scale)
{
    draw_text(center_x - text_width(text, scale) / 2, y,
              text, color, alpha, scale);
}

static void draw_text_right(int right, int y, const char *text,
                            uint32_t color, float alpha, int scale)
{
    draw_text(right - text_width(text, scale), y, text, color, alpha, scale);
}

/* Word-wrap within `width`; `x` is the left edge, or the center when
 * `centered`. Returns the number of lines drawn so the caller can flow
 * following content below it. */
static int draw_wrapped(int x, int y, int width, const char *text,
                        uint32_t color, float alpha, int scale,
                        int max_lines, bool centered)
{
    char line[160] = {0};
    int line_length = 0, lines = 0;
    int max_characters = width / (6 * scale);
    if (max_characters < 1) max_characters = 1;
    const char *word = text;
    while (*word && lines < max_lines) {
        while (*word == ' ') word++;
        const char *end = word;
        while (*end && *end != ' ') end++;
        int word_length = (int)(end - word);
        if (line_length && line_length + 1 + word_length > max_characters) {
            if (centered)
                draw_text_center(x, y + lines * 9 * scale, line, color, alpha,
                                 scale);
            else
                draw_text(x, y + lines * 9 * scale, line, color, alpha, scale);
            lines++;
            line[0] = '\0';
            line_length = 0;
            if (lines >= max_lines) break;
        }
        if (line_length) line[line_length++] = ' ';
        int copy = word_length;
        if (copy > (int)sizeof line - line_length - 1)
            copy = (int)sizeof line - line_length - 1;
        memcpy(line + line_length, word, (size_t)copy);
        line_length += copy;
        line[line_length] = '\0';
        word = end;
    }
    if (line_length && lines < max_lines) {
        if (centered)
            draw_text_center(x, y + lines * 9 * scale, line, color, alpha,
                             scale);
        else
            draw_text(x, y + lines * 9 * scale, line, color, alpha, scale);
        lines++;
    }
    return lines;
}

static void draw_wrapped_text(int x, int y, int width, const char *text,
                              uint32_t color, float alpha, int scale,
                              int max_lines)
{
    draw_wrapped(x, y, width, text, color, alpha, scale, max_lines, false);
}

static int draw_wrapped_center(int cx, int y, int width, const char *text,
                               uint32_t color, float alpha, int scale,
                               int max_lines)
{
    return draw_wrapped(cx, y, width, text, color, alpha, scale, max_lines,
                        true);
}

static Bitmap load_ppm(const char *path)
{
    Bitmap bitmap = {0};
    sr_canvas image;
    if (!sr_load_ppm(&image, path)) return bitmap;
    if (image.w > 4096 || image.h > 4096) {
        sr_canvas_free(&image);
        return bitmap;
    }
    bitmap.w = image.w;
    bitmap.h = image.h;
    bitmap.px = image.px;
    bitmap.ok = true;
    return bitmap;
}

static void free_bitmap(Bitmap *bitmap)
{
    free(bitmap->px);
    memset(bitmap, 0, sizeof *bitmap);
}

static void draw_bitmap_cover(const Bitmap *bitmap, float darken)
{
    if (!bitmap->ok) {
        fill_gradient(0, 0, LW, LH, 0x7B9B9B, 0xD49A51, 1.0f);
        return;
    }
    for (int y = 0; y < LH; y++) {
        int sy = y * bitmap->h / LH;
        for (int x = 0; x < LW; x++) {
            int sx = x * bitmap->w / LW;
            uint32_t color = bitmap->px[sy * bitmap->w + sx];
            if (darken > 0.0f) color = color_mix(color, NIGHT, darken);
            pixel_set(x, y, color);
        }
    }
}

static bool is_chroma(uint32_t color)
{
    int red = (color >> 16) & 255;
    int green = (color >> 8) & 255;
    int blue = color & 255;
    return red > 215 && blue > 215 && green < 90;
}

static void draw_bitmap_scaled(const Bitmap *bitmap, int x, int y,
                               int width, int height, bool flip,
                               uint32_t tint, float tint_amount, float alpha)
{
    if (!bitmap->ok || width <= 0 || height <= 0) return;
    for (int dy = 0; dy < height; dy++) {
        int sy = dy * bitmap->h / height;
        for (int dx = 0; dx < width; dx++) {
            int raw_x = dx * bitmap->w / width;
            int sx = flip ? bitmap->w - 1 - raw_x : raw_x;
            uint32_t color = bitmap->px[sy * bitmap->w + sx];
            if (is_chroma(color)) continue;
            if (tint_amount > 0.0f)
                color = color_mix(color, tint, tint_amount);
            pixel_blend(x + dx, y + dy, color, alpha);
        }
    }
}

/* Blit one cell of a horizontal `frames`-cell atlas, scaled into (x,y,w,h). */
static void draw_bitmap_atlas_frame(const Bitmap *bitmap, int frame, int frames,
                                    int x, int y, int width, int height,
                                    bool flip, uint32_t tint, float tint_amount,
                                    float alpha)
{
    if (!bitmap->ok || width <= 0 || height <= 0 || frames <= 0) return;
    int fw = bitmap->w / frames;
    if (fw <= 0) return;
    frame = clampi(frame, 0, frames - 1);
    int base = frame * fw;
    for (int dy = 0; dy < height; dy++) {
        int sy = dy * bitmap->h / height;
        for (int dx = 0; dx < width; dx++) {
            int raw_x = dx * fw / width;
            int sx = base + (flip ? fw - 1 - raw_x : raw_x);
            uint32_t color = bitmap->px[sy * bitmap->w + sx];
            if (is_chroma(color)) continue;
            if (tint_amount > 0.0f)
                color = color_mix(color, tint, tint_amount);
            pixel_blend(x + dx, y + dy, color, alpha);
        }
    }
}

/* Map a draw pose (0 idle, 1 happy, 2 tired, 3 attack, 4 hit) to an atlas
 * frame. Idle gently alternates idle/walk for life, or naps when exhausted;
 * an attack crouches to wind up then pounces as the lunge extends. */
static int kilix_anim_frame(int pose, float lunge)
{
    switch (pose) {
    case 0: return G.kilix.fatigue > 78 ? KF_NAP
                 : (((int)(G.time * 1.4f) & 1) ? KF_WALK : KF_IDLE);
    case 1: return ((int)(G.time * 5.0f) & 1) ? KF_WALK : KF_IDLE;
    case 2: return KF_NAP;
    case 3: return fabsf(lunge) > 12.0f ? KF_POUNCE : KF_CROUCH;
    case 4: return KF_HURT;
    default: return KF_IDLE;
    }
}

static void draw_ember(float x, float y, float phase, float size)
{
    float pulse = 0.7f + 0.3f * sinf(phase);
    fill_circle(x, y, size * 1.8f, ORANGE, 0.13f * pulse);
    fill_circle(x, y, size, GOLD, 0.8f * pulse);
    fill_circle(x, y, size * 0.42f, 0xFFF4C4, 0.95f * pulse);
}

static void draw_ambient_embers(int count, float strength)
{
    for (int i = 0; i < count; i++) {
        float phase = G.time * (0.7f + (i % 5) * 0.08f) + i * 1.91f;
        float x = fmodf(i * 173.0f + sinf(phase) * 28.0f + 1200.0f, LW + 80.0f) - 40.0f;
        float y = LH - fmodf(i * 71.0f + G.time * (18 + i % 9), LH + 60.0f);
        draw_ember(x, y, phase, 1.3f + (i % 3) * 0.55f * strength);
    }
}

static void draw_progress_bar(int x, int y, int width, int height,
                              float value, float maximum, uint32_t color,
                              const char *label)
{
    rounded_rect(x, y, width, height, height / 2, 0x121719, 0.78f);
    float fraction = maximum > 0 ? clampf(value / maximum, 0.0f, 1.0f) : 0.0f;
    int inner = (int)((width - 4) * fraction);
    if (inner > 0)
        rounded_rect(x + 2, y + 2, inner, height - 4,
                     height / 2 - 2, color, 0.95f);
    if (label)
        draw_text(x + 7, y + (height - 14) / 2, label, CREAM, 1.0f, 2);
}

static void draw_choice(int x, int y, int width, int height, const char *label,
                        const char *hint, bool selected, bool enabled)
{
    uint32_t base = selected ? ORANGE : NIGHT;
    float alpha = selected ? 0.94f : 0.78f;
    rounded_rect(x, y, width, height, 9, base, alpha);
    if (selected) {
        fill_triangle(x - 11, y + height / 2.0f, x - 2, y + height / 2.0f - 7,
                      x - 2, y + height / 2.0f + 7, GOLD, 1.0f);
        fill_rect(x + 5, y + 5, 3, height - 10, GOLD, 0.9f);
    }
    uint32_t text_color = enabled ? CREAM : 0x8D8A83;
    draw_text(x + 15, y + 11, label, text_color, enabled ? 1.0f : 0.65f, 2);
    if (hint && *hint &&
        text_width(label, 2) + text_width(hint, 1) <= width - 42)
        draw_text_right(x + width - 12, y + 13, hint,
                        selected ? 0xFFF5D1 : 0xB9C4B4,
                        enabled ? 0.95f : 0.5f, 1);
}

static void draw_stat_row(int x, int y, int width, StatKind stat, int value)
{
    char amount[24];
    snprintf(amount, sizeof amount, "%d", value);
    draw_text(x, y, STAT_NAMES[stat], CREAM, 0.96f, 1);
    draw_text_right(x + width, y, amount, GOLD, 1.0f, 1);
    int bar_x = x + 67;
    int bar_width = width - 96;
    rounded_rect(bar_x, y + 1, bar_width, 6, 3, 0x0C2024, 0.8f);
    int fill = (int)(bar_width * clampf(value / 500.0f, 0.02f, 1.0f));
    rounded_rect(bar_x, y + 1, fill, 6, 3,
                 stat == STAT_INTELLECT ? CORAL :
                 stat == STAT_SPEED ? 0x67C4B2 : GOLD, 0.95f);
}

static void draw_footer(const char *text)
{
    fill_rect(0, LH - 28, LW, 28, 0x101C22, 0.9f);
    draw_text_center(LW / 2, LH - 20, text, 0xEADCAD, 0.88f, 1);
}

static void draw_header(const char *section)
{
    fill_gradient(0, 0, LW, 46, 0x13252E, 0x1A3038, 0.95f);
    draw_text(22, 14, section, GOLD, 1.0f, 2);
    char date[64];
    snprintf(date, sizeof date, "YEAR %d  %s %02d", game_year(),
             game_season_name(), game_week_of_year());
    draw_text_center(LW / 2, 16, date, CREAM, 0.95f, 1);
    char money[64];
    snprintf(money, sizeof money, "%d G  |  %s LEAGUE",
             G.money, RANK_NAMES[G.kilix.rank]);
    draw_text_right(LW - 22, 16, money, CREAM, 0.95f, 1);
}

static void draw_kilix(float anchor_x, float ground_y, float size, bool flip,
                       int pose)
{
    float bob = sinf(G.time * 2.7f + G.ambient_phase) * 3.0f;
    float squash = 1.0f;
    float stretch = 1.0f;
    float x_shift = 0.0f;
    uint32_t tint = 0xFFFFFF;
    float tint_amount = 0.0f;

    if (G.kilix.fatigue > 75 && pose == 0) {
        bob = fabsf(sinf(G.time * 1.3f)) * 1.5f + 7.0f;
        squash = 0.94f;
        stretch = 0.97f;
    }
    if (pose == 1) {
        float jump = fabsf(sinf(G.time * 5.5f));
        bob -= jump * 22.0f;
        squash = 1.05f - jump * 0.08f;
        stretch = 0.95f + jump * 0.12f;
    } else if (pose == 2) {
        bob += 10.0f;
        squash = 1.08f;
        stretch = 0.88f;
        tint = 0x8395A4;
        tint_amount = 0.17f;
    } else if (pose == 3) {
        /* In battle the lunge tracks the attack phase; elsewhere (Dojo card,
         * Academy montage) G.battle.phase_timer is stale, so repeat a practice
         * pounce on a fixed cadence instead. */
        float lunge_time = G.screen == SCREEN_BATTLE ? G.battle.phase_timer
                                                     : fmodf(G.time, 1.4f);
        float attack = sinf(clampf(lunge_time * 8.0f, 0.0f, 3.14159f));
        x_shift = attack * 92.0f;
        squash = 1.0f + attack * 0.12f;
        stretch = 1.0f - attack * 0.08f;
    } else if (pose == 4) {
        x_shift = sinf(G.time * 54.0f) * 9.0f;
        tint = 0xFFF2C2;
        tint_amount = 0.48f;
    }

    int width = (int)(size * squash);
    int height = (int)(size * stretch);
    float facing_shift = flip ? -x_shift : x_shift;
    int x = (int)(anchor_x - width * 0.5f + facing_shift);
    int y = (int)(ground_y - height + bob);

    fill_ellipse(anchor_x + facing_shift, ground_y + 3.0f,
                 size * 0.27f, size * 0.045f, 0x1A1715, 0.35f);
    if (kilix_atlas.ok) {
        /* Animated path: the atlas frame already carries the pose deformation,
         * so draw at a neutral size and keep only the lunge shift, the idle bob
         * and the hit flash. */
        int fx = (int)(anchor_x - size * 0.5f + facing_shift);
        int fy = (int)(ground_y - size + bob);
        float flash = pose == 4 ? tint_amount : 0.0f;
        draw_bitmap_atlas_frame(&kilix_atlas, kilix_anim_frame(pose, x_shift),
                                KILIX_FRAMES, fx, fy, (int)size, (int)size,
                                flip, tint, flash, 1.0f);
    } else {
        draw_bitmap_scaled(&kilix_sprite, x, y, width, height, flip,
                           tint, tint_amount, 1.0f);
    }

    for (int i = 0; i < 5; i++) {
        float phase = G.time * (2.0f + i * 0.12f) + i * 1.31f;
        float ex = anchor_x + facing_shift + (flip ? 1 : -1) * size * 0.31f +
                   sinf(phase) * 9.0f;
        float ey = ground_y - size * 0.53f -
                   fmodf(G.time * (11 + i * 2) + i * 21.0f, size * 0.25f);
        draw_ember(ex, ey, phase, 1.2f + (i & 1));
    }
}

static void draw_opponent(int index, float anchor_x, float ground_y,
                          float size, int pose)
{
    index = clampi(index, 0, OPPONENT_COUNT - 1);
    float bob = sinf(G.time * (2.0f + index * 0.12f) + index) * 3.0f;
    float shake = pose == 4 ? sinf(G.time * 49.0f) * 8.0f : 0.0f;
    float lunge = 0.0f;
    if (pose == 3)
        lunge = -sinf(clampf(G.battle.phase_timer * 8.0f, 0.0f, 3.14159f)) * 82.0f;
    /* Attack-recoil squash and a pale flash while taking a hit, mirroring the
     * Kilix so both fighters read the same way. */
    float squash = 1.0f, stretch = 1.0f;
    uint32_t tint = 0xFFFFFF;
    float tint_amount = 0.0f;
    if (pose == 3) { squash = 1.06f; stretch = 0.95f; }
    else if (pose == 4) { tint = 0xFFC9C2; tint_amount = 0.5f; }

    float x = anchor_x + shake + lunge;
    float y = ground_y + bob;
    fill_ellipse(x, ground_y + 4, size * 0.31f, size * 0.055f,
                 0x171513, 0.36f);

    int w = (int)(size * squash);
    int h = (int)(size * stretch);
    int px = (int)(x - w * 0.5f), py = (int)(y - h);
    /* Rival plates are ground-anchored like the Kilix, so scaling the whole
     * canvas to `size` lands the feet on the arena floor; the art already
     * faces left toward the player, so no flip. Prefer the animation atlas,
     * fall back to the single plate, then to a colored blob. */
    if (opponent_atlas[index].ok) {
        int frame;
        if (pose == 3)      frame = fabsf(lunge) > 12.0f ? KF_POUNCE : KF_CROUCH;
        else if (pose == 4) frame = KF_HURT;
        else                frame = ((int)(G.time * 1.2f) & 1) ? KF_WALK : KF_IDLE;
        draw_bitmap_atlas_frame(&opponent_atlas[index], frame, KILIX_FRAMES,
                                px, py, w, h, false, tint, tint_amount, 1.0f);
    } else if (opponent_sprites[index].ok) {
        draw_bitmap_scaled(&opponent_sprites[index], px, py, w, h, false,
                           tint, tint_amount, 1.0f);
    } else {
        fill_ellipse(x, y - size * 0.32f, size * 0.34f, size * 0.26f,
                     OPPONENTS[index].color, 1.0f);
    }
}

static int ranch_pose(void)
{
    if (G.kilix.fatigue >= 82 || G.kilix.stress >= 85) return 2;
    if (G.kilix.bond >= 75 && fmodf(G.time, 9.0f) < 1.2f) return 1;
    return 0;
}

static void draw_title(void)
{
    draw_bitmap_cover(&ranch_background, 0.16f);
    fill_gradient(0, 0, LW, LH, 0x132A35, 0x441D16, 0.24f);
    draw_ambient_embers(30, 1.0f);
    draw_kilix(738, 505, 430, false, 0);

    panel(48, 58, 487, 423, NIGHT, 0.91f);
    draw_text_shadow(83, 94, "KILIX", GOLD, 1.0f, 6);
    draw_text_shadow(84, 147, "RANCHER", CREAM, 1.0f, 4);
    draw_text(85, 190, "RAISE THE SPARK. WIN THE CROWN.",
              0xD8B982, 0.95f, 1);
    fill_rect(84, 213, 392, 2, ORANGE, 0.85f);

    static const char *options[] = {
        "CONTINUE RANCH", "NEW RANCH", "FIELD GUIDE", "QUIT"
    };
    for (int i = 0; i < 4; i++) {
        bool enabled = i != 0 || G.save_exists;
        const char *hint = i == 0 && !G.save_exists ? "NO SAVE" : "";
        draw_choice(90, 235 + i * 51, 360, 41, options[i], hint,
                    G.title_cursor == i, enabled);
    }
    draw_text(83, 454, "ORIGINAL FIRE-KITTEN RAISING ADVENTURE",
              0xB9C6B1, 0.78f, 1);
    draw_footer("ARROWS / WASD  MOVE     ENTER  CHOOSE     M  SOUND");
}

static void draw_naming(void)
{
    draw_bitmap_cover(&ranch_background, 0.34f);
    draw_ambient_embers(18, 0.8f);
    panel(155, 76, 650, 398, NIGHT, 0.94f);
    draw_kilix(330, 447, 315, false, 0);
    draw_text(462, 113, "A NEW SPARK", GOLD, 1.0f, 3);
    draw_text(463, 151, "SPECIES: KILIX", CREAM, 0.9f, 1);
    draw_text(463, 170, "TYPE: FIRE KITTEN", 0xD4B27A, 0.9f, 1);
    draw_wrapped_text(463, 207, 285,
                      "THIS LITTLE FLAME HAS CHOSEN YOUR HEARTH. WHAT WILL YOU CALL THEM?",
                      CREAM, 0.95f, 2, 4);
    rounded_rect(459, 309, 300, 55, 10, 0x0E171C, 0.95f);
    fill_rect(468, 354, 280, 2, ORANGE, 0.9f);
    char name[32];
    snprintf(name, sizeof name, "%s%s", G.name_input,
             fmodf(G.time, 1.0f) < 0.55f ? "_" : "");
    draw_text(477, 326, name, GOLD, 1.0f, 2);
    draw_text(463, 390, "ENTER TO BEGIN  |  ESC TO RETURN",
              0xB7C5B7, 0.82f, 1);
    draw_footer("TYPE A NAME     BACKSPACE  ERASE     ENTER  ADOPT");
}

static void draw_ranch_status(void)
{
    panel(706, 68, 230, 390, NIGHT, 0.89f);
    draw_text(728, 89, G.kilix.name, GOLD, 1.0f, 3);
    draw_text(730, 120, "KILIX  //  FIRE KITTEN", CREAM, 0.75f, 1);
    char condition[64];
    snprintf(condition, sizeof condition, "MOOD: %s", game_condition());
    draw_text(730, 143, condition,
              G.kilix.fatigue > 75 ? CORAL : 0x8FD1B5, 1.0f, 1);
    fill_rect(726, 166, 190, 2, 0xC7A65B, 0.6f);
    for (int stat = 0; stat < STAT_COUNT; stat++)
        draw_stat_row(728, 183 + stat * 27, 185, (StatKind)stat,
                      G.kilix.stats.value[stat]);

    fill_rect(726, 348, 190, 2, 0xC7A65B, 0.6f);
    char value[64];
    snprintf(value, sizeof value, "BOND  %d%%", G.kilix.bond);
    draw_progress_bar(728, 354, 185, 18, G.kilix.bond, 100, 0xE98A5C, value);
    snprintf(value, sizeof value, "FATIGUE  %d%%", G.kilix.fatigue);
    draw_progress_bar(728, 376, 185, 18, G.kilix.fatigue, 100,
                      G.kilix.fatigue > 70 ? CORAL : 0x6CA69C, value);
    snprintf(value, sizeof value, "HUNGER  %d%%", G.kilix.hunger);
    draw_progress_bar(728, 398, 185, 18, G.kilix.hunger, 100,
                      G.kilix.hunger >= HUNGER_STARVE ? 0xE0533A :
                      G.kilix.hunger >= HUNGER_WARN ? CORAL : 0x6CA69C, value);
    snprintf(value, sizeof value, "RATING %d   AGE %dY %dW",
             game_overall_rating(), G.kilix.age_weeks / 48,
             G.kilix.age_weeks % 48);
    draw_text(730, 422, value, 0xD7C18C, 0.85f, 1);
    /* Rent countdown: how many weeks until the collector next calls. */
    int weeks_left = G.rent_paid_weeks - G.total_weeks;
    if (weeks_left < 0) weeks_left = 0;
    if (weeks_left == 0)
        snprintf(value, sizeof value, "RENT %d G  DUE NOW", RENT_AMOUNT);
    else
        snprintf(value, sizeof value, "RENT %d G  IN %d WK", RENT_AMOUNT, weeks_left);
    draw_text(730, 440, value, weeks_left <= 1 ? CORAL : 0xE0B060, 0.9f, 1);
}

static void draw_ranch(void)
{
    draw_bitmap_cover(&ranch_background, 0.03f);
    draw_ambient_embers(16, 0.7f);
    draw_header("HEARTHSIDE RANCH");

    panel(24, 68, 264, 390, NIGHT, 0.89f);
    draw_text(48, 86, "THIS WEEK", GOLD, 1.0f, 2);
    draw_text(49, 108, "ONE CHOICE MOVES TIME FORWARD", CREAM, 0.66f, 1);
    static const char *labels[] = {
        "DRILLS", "ACADEMY", "DOJO", "BANK", "CATNAP", "CARE BASKET",
        "FESTIVAL ARENA", "JOURNAL", "SAVE & STAY"
    };
    static const char *hints[] = {
        "+ STATS", "PAID +STAT", "LEARN", "RENT", "- FATIGUE", "FOOD",
        "LEAGUE", "INFO", "SAVE"
    };
    for (int i = 0; i < 9; i++)
        draw_choice(48, 120 + i * 37, 214, 31, labels[i], hints[i],
                    G.cursor == i, true);

    int pose = ranch_pose();
    draw_kilix(505, 468, 360, false, pose);
    fill_ellipse(510, 461, 124, 12, 0x16110D, 0.17f);

    rounded_rect(316, 386, 358, 72, 13, 0xFFF0C9, 0.91f);
    fill_triangle(492, 386, 513, 366, 535, 386, 0xFFF0C9, 0.91f);
    const char *message = G.toast_timer > 0.0f ? G.toast :
        G.kilix.hunger >= HUNGER_STARVE ?
            "MY TUMMY ECHOES... I CAN'T GROW LIKE THIS. STEW?" :
        G.kilix.hunger >= HUNGER_WARN ? "COULD WE SHARE A SNACK SOON?" :
        G.kilix.fatigue > 78 ? "MY FLAME FEELS SMALL... MAYBE A CATNAP?" :
        G.kilix.stress > 74 ? "CAN WE SLOW DOWN THIS WEEK?" :
        G.kilix.bond > 70 ? "WHATEVER WE DO, LET'S DO IT TOGETHER!" :
        "THE AIR SMELLS LIKE A BRAND-NEW ADVENTURE.";
    draw_wrapped_text(336, 404, 320, message, INK, 1.0f, 1, 3);

    draw_ranch_status();
    if (G.autosave_flash > 0.0f) {
        rounded_rect(787, 474, 147, 30, 8, NIGHT, 0.84f);
        draw_text(803, 485, "EMBER SAVED", GOLD, 0.95f, 1);
    }
    draw_footer("ARROWS / WASD  CHOOSE     ENTER  CONFIRM     J  JOURNAL     Q  QUIT");
}

static void draw_drill_icon(int index, float cx, float cy)
{
    float pulse = 1.0f + sinf(G.time * 2.0f) * 0.03f;
    /* Prefer a generated emblem (chosen by the drill's primary stat); fall back
     * to the procedural icon below when the sprite is absent. */
    int stat = clampi(DRILLS[clampi(index, 0, DRILL_COUNT - 1)].primary,
                      0, STAT_COUNT - 1);
    if (drill_icons[stat].ok) {
        int s = (int)(96 * pulse);
        draw_bitmap_scaled(&drill_icons[stat], (int)(cx - s / 2),
                           (int)(cy - s / 2), s, s, false, 0xFFFFFF, 0.0f, 1.0f);
        return;
    }
    switch (index) {
    case 0:
        fill_rect((int)(cx - 64), (int)(cy + 24), 128, 12, 0x5C493B, 0.9f);
        fill_circle(cx - 33, cy, 27 * pulse, 0x3A3331, 1.0f);
        fill_circle(cx + 18, cy - 9, 35 * pulse, 0x51433B, 1.0f);
        draw_ember(cx + 42, cy - 36, G.time * 3.0f, 4.0f);
        break;
    case 1:
        rounded_rect((int)(cx - 52), (int)(cy - 36), 104, 72, 5,
                     0x7B3D2F, 1.0f);
        fill_rect((int)cx - 2, (int)(cy - 31), 4, 61, GOLD, 0.8f);
        draw_ember(cx, cy - 57, G.time * 3.5f, 5.0f);
        break;
    case 2:
        for (int i = 0; i < 4; i++) {
            float angle = G.time * 0.8f + i * 1.57f;
            draw_ember(cx + cosf(angle) * 54, cy + sinf(angle) * 32,
                       angle * 2, 3.5f);
        }
        fill_ellipse(cx, cy + 31, 74, 9, 0x604529, 0.75f);
        break;
    case 3:
        fill_rect((int)(cx - 8), (int)(cy - 51), 16, 94, 0x5B4636, 1.0f);
        for (int i = -2; i <= 2; i++)
            fill_rect((int)(cx - 55), (int)(cy + i * 17), 110, 7,
                      0x865D3A, 0.9f);
        draw_ember(cx + 7, cy - 61, G.time * 3, 4.0f);
        break;
    case 4:
        fill_circle(cx, cy, 58, 0x4A2D25, 1.0f);
        fill_circle(cx, cy, 45, CREAM, 1.0f);
        fill_circle(cx, cy, 29, CORAL, 1.0f);
        fill_circle(cx, cy, 12, GOLD, 1.0f);
        draw_line(cx - 90, cy + 55, cx + 80, cy - 48, 5, NIGHT, 0.8f);
        break;
    default:
        rounded_rect((int)(cx - 36), (int)(cy - 50), 72, 100, 8,
                     0xDFB55C, 0.95f);
        fill_circle(cx, cy - 13, 18, ORANGE, 1.0f);
        fill_circle(cx, cy - 13, 8, GOLD, 1.0f);
        draw_line(cx, cy + 13, cx, cy + 38, 4, INK, 0.65f);
        break;
    }
}

static void draw_training(void)
{
    draw_bitmap_cover(&ranch_background, 0.24f);
    draw_header("TRAINING YARD");
    panel(24, 67, 400, 425, NIGHT, 0.91f);
    draw_text(46, 87, "CHOOSE A DRILL", GOLD, 1.0f, 2);
    draw_text(47, 112, "SUCCESS DEPENDS ON BOND AND ENERGY", CREAM, 0.67f, 1);
    for (int i = 0; i < DRILL_COUNT; i++) {
        char hint[32];
        snprintf(hint, sizeof hint, "+%s", STAT_NAMES[DRILLS[i].primary]);
        draw_choice(47, 137 + i * 55, 350, 46, DRILLS[i].name, hint,
                    G.drill_cursor == i, true);
    }

    panel(455, 67, 480, 425, 0x203039, 0.91f);
    int index = clampi(G.drill_cursor, 0, DRILL_COUNT - 1);
    draw_text(482, 88, DRILLS[index].name, GOLD, 1.0f, 3);
    draw_text(484, 122, DRILLS[index].subtitle, CREAM, 0.76f, 1);
    draw_drill_icon(index, 758, 226);
    draw_kilix(592, 337, 195, false, 0);
    draw_wrapped_text(487, 349, 412, DRILLS[index].description,
                      CREAM, 0.92f, 1, 3);
    char detail[128];
    snprintf(detail, sizeof detail, "%s +%d-%d   %s +1-%d",
             STAT_NAMES[DRILLS[index].primary], DRILLS[index].min_gain,
             DRILLS[index].max_gain, STAT_NAMES[DRILLS[index].secondary],
             (DRILLS[index].max_gain + 1) / 2);
    draw_text(487, 400, detail, 0x9DD7BE, 0.95f, 1);
    /* The multiplier applied to the drill's mini-game score (game.c). */
    int condition = (int)lroundf(game_drill_condition(index) * 100.0f);
    snprintf(detail, sizeof detail, "CONDITION %d%%   FATIGUE +%d   STRESS +%d",
             condition, DRILLS[index].fatigue, DRILLS[index].stress);
    draw_text(487, 425, detail,
              condition < 90 ? CORAL : GOLD, 0.95f, 1);
    if (G.kilix.fatigue >= 82)
        draw_text(487, 446, "TOO TIRED - TAKE A CATNAP FIRST", CORAL, 1.0f, 1);
    draw_footer("UP / DOWN  SELECT     ENTER  TRAIN ONE WEEK     ESC  RANCH");
}

static void draw_care(void)
{
    draw_bitmap_cover(&ranch_background, 0.28f);
    draw_header("CARE BASKET");
    panel(37, 75, 475, 395, NIGHT, 0.92f);
    draw_text(62, 96, "A TREAT FOR THIS WEEK", GOLD, 1.0f, 2);
    draw_text(63, 123, "GOOD CARE BUILDS A LONGER, BRIGHTER BOND", CREAM, 0.7f, 1);
    for (int i = 0; i < ITEM_COUNT; i++) {
        char hint[24];
        snprintf(hint, sizeof hint, "%d G", ITEMS[i].cost);
        draw_choice(64, 158 + i * 67, 420, 55, ITEMS[i].name, hint,
                    G.care_cursor == i, G.money >= ITEMS[i].cost);
        draw_text(82, 193 + i * 67, ITEMS[i].description,
                  0xB9C6B7, G.money >= ITEMS[i].cost ? 0.8f : 0.45f, 1);
    }

    panel(548, 75, 375, 395, 0x24353A, 0.9f);
    int index = clampi(G.care_cursor, 0, ITEM_COUNT - 1);
    draw_kilix(620, 430, 210, false, ranch_pose());
    /* Show the selected treat as a sprite on its own little shelf. */
    if (care_sprites[index].ok) {
        rounded_rect(742, 118, 150, 150, 14, 0x101B20, 0.85f);
        draw_bitmap_scaled(&care_sprites[index], 754, 128, 126, 126, false,
                           0xFFFFFF, 0.0f, 1.0f);
        draw_text_center(817, 250, ITEMS[index].name, GOLD, 0.72f, 1);
    }
    rounded_rect(578, 352, 315, 87, 12, CREAM, 0.92f);
    char effects[160];
    snprintf(effects, sizeof effects,
             "FATIGUE %d   STRESS %d   BOND %+d   FORM %+d",
             ITEMS[index].fatigue, ITEMS[index].stress,
             ITEMS[index].bond, ITEMS[index].form);
    /* Satiety is what separates food from grooming: show it. */
    if (ITEMS[index].satiety > 0) {
        size_t used = strlen(effects);
        snprintf(effects + used, sizeof effects - used,
                 "   HUNGER -%d", ITEMS[index].satiety);
    }
    draw_wrapped_text(598, 371, 275, effects, INK, 1.0f, 1, 3);
    draw_footer("UP / DOWN  SELECT     ENTER  SHARE & ADVANCE WEEK     ESC  RANCH");
}

static void draw_arena(void)
{
    draw_bitmap_cover(&arena_background, 0.10f);
    draw_header("FESTIVAL ARENA");
    panel(25, 69, 300, 398, NIGHT, 0.91f);
    draw_text(49, 91, "LEAGUE BOARD", GOLD, 1.0f, 2);
    draw_text(50, 117, "BEAT YOUR RIVAL - OR REMATCH FOR COIN", CREAM, 0.69f, 1);
    for (int i = 0; i < OPPONENT_COUNT; i++) {
        char label[64];
        snprintf(label, sizeof label, "%s  %s", RANK_NAMES[i], OPPONENTS[i].name);
        const char *hint = i == G.kilix.rank ? "READY" :
                           i < G.kilix.rank ? "REMATCH" : "LOCKED";
        draw_choice(49, 145 + i * 49, 252, 40, label, hint,
                    G.arena_cursor == i, i <= G.kilix.rank);
    }

    int index = clampi(G.arena_cursor, 0, OPPONENT_COUNT - 1);
    panel(650, 69, 285, 398, 0x26343A, 0.91f);
    draw_text_center(792, 91, OPPONENTS[index].name, GOLD, 1.0f, 3);
    draw_text_center(792, 126, OPPONENTS[index].species, CREAM, 0.82f, 1);
    draw_text_center(792, 145, OPPONENTS[index].epithet, 0xD6B97F, 0.82f, 1);
    draw_opponent(index, 792, 380, 255, 0);
    char value[64];
    if (index < G.kilix.rank)      /* exhibition rematch pays half */
        snprintf(value, sizeof value, "PURSE  %d G", OPPONENTS[index].prize / 2);
    else
        snprintf(value, sizeof value, "PRIZE  %d G", OPPONENTS[index].prize);
    draw_text_center(792, 409, value, GOLD, 1.0f, 2);
    snprintf(value, sizeof value, "RATING  %d",
             (OPPONENTS[index].stats.value[0] +
              OPPONENTS[index].stats.value[1] +
              OPPONENTS[index].stats.value[2] +
              OPPONENTS[index].stats.value[3] +
              OPPONENTS[index].stats.value[4] +
              OPPONENTS[index].stats.value[5]) / 6);
    draw_text_center(792, 438, value, CREAM, 0.83f, 1);

    draw_kilix(477, 445, 300, false, 0);
    rounded_rect(347, 77, 267, 78, 11, CREAM, 0.91f);
    char challenge[128];
    if (index == G.kilix.rank)
        snprintf(challenge, sizeof challenge, "%s IS YOUR %s LEAGUE RIVAL. READY?",
                 OPPONENTS[index].name, RANK_NAMES[index]);
    else if (index < G.kilix.rank)
        snprintf(challenge, sizeof challenge,
                 "%s WILL SPAR AGAIN FOR HALF THE PURSE.",
                 OPPONENTS[index].name);
    else
        snprintf(challenge, sizeof challenge, "WIN %s LEAGUE TO UNLOCK THIS RIVAL.",
                 RANK_NAMES[index - 1]);
    draw_wrapped_text(365, 96, 230, challenge, INK, 1.0f, 1, 3);
    draw_footer("UP / DOWN  INSPECT RIVAL     ENTER  CHALLENGE     ESC  RANCH");
}

static void draw_flame_burst(float x, float y, float amount)
{
    amount = clampf(amount, 0.0f, 1.0f);
    for (int i = 0; i < 18; i++) {
        float angle = (float)i / 18.0f * 6.28318f + i * 0.37f;
        float radius = (18.0f + (i % 5) * 8.0f) * amount;
        float px = x + cosf(angle) * radius;
        float py = y + sinf(angle) * radius * 0.7f;
        draw_ember(px, py, angle + G.time * 5.0f,
                   (2.0f + i % 4) * amount);
    }
    fill_circle(x, y, 33.0f * amount, ORANGE, 0.24f * amount);
    fill_circle(x, y, 17.0f * amount, GOLD, 0.34f * amount);
}

static void draw_battle_hud(void)
{
    fill_gradient(0, 0, LW, 94, 0x10232D, 0x20343A, 0.95f);
    char label[96];
    snprintf(label, sizeof label, "%s  //  KILIX", G.kilix.name);
    draw_text(28, 17, label, CREAM, 1.0f, 2);
    /* Defensive clamp: no live path reaches here with an out-of-range
     * opponent today, but the other battle draws guard it, so match them. */
    int opponent = clampi(G.battle.opponent, 0, OPPONENT_COUNT - 1);
    snprintf(label, sizeof label, "%s  //  %s",
             OPPONENTS[opponent].name,
             OPPONENTS[opponent].species);
    draw_text_right(LW - 28, 17, label, CREAM, 1.0f, 2);

    draw_progress_bar(27, 51, 330, 24, G.battle.player_hp,
                      G.battle.player_max_hp,
                      G.battle.player_hp < G.battle.player_max_hp * 0.25f ? CORAL : 0x6AC18D,
                      "HEART");
    draw_progress_bar(LW - 357, 51, 330, 24, G.battle.enemy_hp,
                      G.battle.enemy_max_hp,
                      G.battle.enemy_hp < G.battle.enemy_max_hp * 0.25f ? CORAL : 0xD79854,
                      "HEART");
    rounded_rect(LW / 2 - 47, 13, 94, 62, 12, 0x0D171B, 0.94f);
    draw_text_center(LW / 2, 22, "TIME", 0xB8C8BB, 0.9f, 1);
    snprintf(label, sizeof label, "%02d", clampi((int)ceilf(G.battle.timer), 0, 99));
    draw_text_center(LW / 2, 40, label, GOLD, 1.0f, 3);
}

static void draw_move_cards(void)
{
    fill_gradient(0, 432, LW, 108, 0x10212A, 0x081217, 0.98f);
    char guts[48];
    snprintf(guts, sizeof guts, "WILL %02d", (int)G.battle.player_guts);
    draw_progress_bar(24, 447, 162, 23, G.battle.player_guts, 100.0f,
                      GOLD, guts);
    draw_text(31, 482, "LEFT / RIGHT", CREAM, 0.86f, 1);
    draw_text(31, 498, "CHANGE RANGE", CREAM, 0.67f, 1);

    for (int i = 0; i < MOVE_COUNT; i++) {
        int x = 202 + i * 183;
        bool selected = G.battle.selected_move == i;
        bool known = (G.moves_known & (1u << i)) != 0;
        bool affordable = G.battle.player_guts >= MOVES[i].cost;
        float range = G.battle.distance * 100.0f;
        bool in_range = range >= MOVES[i].min_range && range <= MOVES[i].max_range;
        bool ready = known && affordable && in_range;
        rounded_rect(x, 445, 168, 72, 9,
                     selected ? ORANGE : 0x20333A,
                     selected ? 0.96f : (known ? 0.9f : 0.72f));
        if (selected) fill_rect(x + 5, 451, 3, 59, GOLD, 0.95f);
        char number[16];
        snprintf(number, sizeof number, "%d", i + 1);
        fill_circle(x + 21, 464, 12, selected ? GOLD : 0x41575C, 1.0f);
        draw_text_center(x + 21, 460, number, INK, 1.0f, 1);
        /* Grays and coral read well on the dark cards but vanish on the
         * selected card's bright orange, so that card gets dark-ink text. */
        draw_text(x + 41, 456, MOVES[i].name,
                  ready ? CREAM : (selected ? INK : 0x8E9390),
                  known ? 1.0f : 0.55f, 1);
        if (!known) {
            /* Locked until bought at the Dojo. */
            char lock[40];
            snprintf(lock, sizeof lock, "LOCKED - %d g", MOVES[i].price);
            draw_text(x + 16, 485, lock, selected ? INK : 0x9CA49E,
                      selected ? 0.85f : 0.7f, 1);
            draw_text(x + 16, 501, "LEARN AT THE DOJO",
                      selected ? INK : 0xCE9468, 0.85f, 1);
            continue;
        }
        char detail[48];
        snprintf(detail, sizeof detail, "%d WILL  %02d-%02d RANGE",
                 MOVES[i].cost, (int)MOVES[i].min_range,
                 (int)MOVES[i].max_range);
        draw_text(x + 16, 485, detail,
                  selected ? INK : (in_range ? GOLD : 0x9CA49E),
                  in_range ? 0.95f : (selected ? 0.8f : 0.62f), 1);
        draw_text(x + 16, 501,
                  !in_range ? "OUT OF RANGE" : !affordable ? "NEED WILL" : "READY",
                  selected ? (ready ? 0x1E5232 : 0x651D14)
                           : (ready ? 0x9DDBB4 : CORAL), 0.95f, 1);
    }
}

static void draw_distance_gauge(void)
{
    int x = 328;
    int y = 404;
    int width = 304;
    rounded_rect(x, y, width, 12, 6, 0x0D171B, 0.83f);
    fill_rect(x + 6, y + 4, width - 12, 4, 0xD2B269, 0.7f);
    int marker = x + 6 + (int)((width - 12) * clampf(G.battle.distance, 0.0f, 1.0f));
    fill_triangle(marker, y - 5, marker - 7, y + 2, marker + 7, y + 2,
                  GOLD, 1.0f);
    draw_text(x - 52, y + 2, "CLOSE", CREAM, 0.83f, 1);
    draw_text(x + width + 12, y + 2, "FAR", CREAM, 0.83f, 1);
}

static void draw_battle(void)
{
    draw_bitmap_cover(&arena_background, 0.03f);
    if (G.battle.shake > 0.0f)
        fill_rect(0, 0, LW, LH, 0xFFF0C8, G.battle.shake * 0.06f);
    draw_battle_hud();

    float closeness = 1.0f - clampf(G.battle.distance, 0.0f, 1.0f);
    float player_x = 245.0f + closeness * 115.0f;
    float enemy_x = 715.0f - closeness * 115.0f;
    /* Impact camera-shake: jitter the fighters and effects on a landed hit. */
    float shk = G.battle.shake;
    float sx = shk > 0.0f ? sinf(G.time * 88.0f) * shk * 20.0f : 0.0f;
    float sy = shk > 0.0f ? cosf(G.time * 71.0f) * shk * 12.0f : 0.0f;
    int player_pose = G.battle.phase == BATTLE_PLAYER_ATTACK ? 3 :
                      G.battle.phase == BATTLE_ENEMY_ATTACK ? 4 :
                      G.battle.phase == BATTLE_FINISHED && G.battle.winner > 0 ? 1 :
                      G.battle.phase == BATTLE_FINISHED ? 2 : 0;
    int enemy_pose = G.battle.phase == BATTLE_ENEMY_ATTACK ? 3 :
                     G.battle.phase == BATTLE_PLAYER_ATTACK ? 4 :
                     G.battle.phase == BATTLE_FINISHED && G.battle.winner < 0 ? 1 :
                     G.battle.phase == BATTLE_FINISHED ? 4 : 0;
    draw_kilix(player_x + sx, 395 + sy, 275, false, player_pose);
    draw_opponent(G.battle.opponent, enemy_x + sx, 390 + sy, 245, enemy_pose);

    if ((G.battle.phase == BATTLE_PLAYER_ATTACK ||
         G.battle.phase == BATTLE_ENEMY_ATTACK) && G.battle.hit) {
        bool by_player = G.battle.phase == BATTLE_PLAYER_ATTACK;
        float target_x = by_player ? enemy_x : player_x;
        float amount = sinf(clampf(G.battle.phase_timer * 9.0f, 0.0f, 3.14159f));
        draw_flame_burst(target_x + sx, 287 + sy, amount);
        if (G.battle.last_damage > 0) {           /* floating damage number */
            float rise = (1.0f - clampf(G.battle.phase_timer / 0.38f, 0, 1)) * 48;
            char dmg[16];
            snprintf(dmg, sizeof dmg, "-%d", G.battle.last_damage);
            draw_text_center((int)(target_x + sx), (int)(252 + sy - rise), dmg,
                             by_player ? GOLD : CORAL, 1.5f, 3);
        }
    }

    /* Auto-battle badge (while active) and a persistent hint. */
    if (G.battle.autopilot && G.battle.phase != BATTLE_READY &&
        G.battle.phase != BATTLE_FINISHED) {
        rounded_rect(LW / 2 - 58, 152, 116, 26, 8, 0x7A3B1E, 0.92f);
        draw_text_center(LW / 2, 161, "AUTO-BATTLE", GOLD, 0.76f, 2);
    }
    draw_text(26, 158, G.battle.autopilot ? "[V] AUTO ON" : "[V] AUTO",
              G.battle.autopilot ? GOLD : 0x8FA0A8, 0.72f, 1);

    if (G.battle.callout[0]) {
        rounded_rect(304, 105, 352, 41, 10, NIGHT, 0.84f);
        draw_text_center(480, 119, G.battle.callout,
                         G.battle.hit ? GOLD : CREAM, 1.0f, 2);
    }
    draw_distance_gauge();
    draw_move_cards();

    if (G.battle.phase == BATTLE_READY) {
        panel(310, 168, 340, 138, NIGHT, 0.95f);
        draw_text_center(480, 190, "READY YOUR FLAME", GOLD, 1.0f, 2);
        draw_text_center(480, 223, "BUILD WILL. MIND YOUR RANGE.", CREAM, 0.88f, 1);
        draw_text_center(480, 250, "PRESS ENTER TO FIGHT", 0x9ED5B7, 0.92f, 1);
        draw_text_center(480, 274, "PRESS V FOR AUTO-BATTLE", 0xE0A45A, 0.82f, 1);
    } else if (G.battle.phase == BATTLE_FINISHED) {
        panel(306, 158, 348, 145, NIGHT, 0.96f);
        const char *result = G.battle.winner > 0 ? "VICTORY!" :
                             G.battle.winner < 0 ? "DEFEAT" : "TIME! DRAW";
        draw_text_center(480, 181, result,
                         G.battle.winner > 0 ? GOLD : CORAL, 1.0f, 4);
        char detail[96];
        snprintf(detail, sizeof detail, "LAST IMPACT  %d", G.battle.last_damage);
        draw_text_center(480, 236, detail, CREAM, 0.8f, 1);
        draw_text_center(480, 266, "ENTER FOR RESULTS", 0xA4D6BA, 1.0f, 1);
    }
}

static void draw_event(void)
{
    bool arena_event = G.event.kind == EVENT_BATTLE_RESULT ||
                       G.event.kind == EVENT_RANK_UP;
    draw_bitmap_cover(arena_event ? &arena_background : &ranch_background,
                      0.28f);
    draw_ambient_embers(24, 1.0f);

    if (G.event.kind == EVENT_TRAIN) {
        draw_drill_icon(G.event.index, 245, 350);
        draw_kilix(700, 475, 320, false,
                   G.event.success ? (G.event.great ? 1 : 0) : 2);
    } else if (G.event.kind == EVENT_REST) {
        fill_ellipse(690, 445, 145, 23, 0x17110D, 0.26f);
        draw_kilix(690, 475, 340, false, 2);
        for (int i = 0; i < 3; i++) {
            char z[8] = "Z";
            draw_text(785 + i * 24, 285 - i * 31, z,
                      CREAM, 0.55f + i * 0.15f, 2 + i);
        }
    } else if (G.event.kind == EVENT_FEED) {
        draw_kilix(700, 475, 335, false, 1);
        int ci = clampi(G.event.index, 0, ITEM_COUNT - 1);
        float bob = sinf(G.time * 2.2f) * 6.0f;
        if (care_sprites[ci].ok) {
            draw_bitmap_scaled(&care_sprites[ci], 168, (int)(232 + bob),
                               170, 170, false, 0xFFFFFF, 0.0f, 1.0f);
        } else {
            /* Fallback bowl sits where the sprite would, under the embers. */
            fill_circle(253, (int)(340 + bob), 42, 0xB66B3F, 1.0f);
            fill_circle(253, (int)(334 + bob), 34, CREAM, 1.0f);
        }
        for (int i = 0; i < 4; i++)
            draw_ember(250 + i * 12, 300 + (i & 1) * 8, G.time * 3 + i, 2.2f);
    } else if (G.event.kind == EVENT_RENT || G.event.kind == EVENT_EVICTION) {
        bool evicted = G.event.kind == EVENT_EVICTION;
        float bob = sinf(G.time * 2.0f) * 5.0f;
        /* The ranch collector calls for the monthly rent. */
        if (rent_sprite.ok)
            draw_bitmap_scaled(&rent_sprite, 150, (int)(214 + bob), 250, 250,
                               false, evicted ? 0x8A6A6A : 0xFFFFFF,
                               evicted ? 0.28f : 0.0f, 1.0f);
        else {
            fill_circle(250, 336, 58, 0x8A6A44, 1.0f);
            draw_text_center(250, 320, "G", GOLD, 1.0f, 4);
        }
        draw_kilix(720, 475, 320, false, evicted ? 2 : 0);
        if (!evicted)
            for (int i = 0; i < 5; i++)
                draw_ember(432 + i * 10, 262 + (i & 1) * 12, G.time * 3 + i, 2.4f);
    } else {
        draw_kilix(315, 462, 285, false,
                   G.event.success || G.event.kind == EVENT_RANK_UP ? 1 : 2);
        draw_opponent(G.battle.opponent, 740, 452, 245,
                      G.event.success ? 4 : 1);
    }

    float reveal = smoothstep(0.15f, 0.55f,
                              G.event.duration > 0.0f ?
                              G.event.timer / G.event.duration : 1.0f);
    panel(170, 55, 620, 150, NIGHT, 0.93f * reveal);
    draw_text_center(480, 79, G.event.title[0] ? G.event.title : "A WEEK PASSES",
                     G.event.success ? GOLD : CORAL, reveal, 3);
    draw_wrapped_text(205, 125, 550, G.event.detail,
                      CREAM, 0.94f * reveal, 1, 4);

    if (G.event.kind == EVENT_TRAIN && G.event.success) {
        char gains[128];
        if (G.event.gain_secondary > 0)   /* drills give two stats; coaching, one */
            snprintf(gains, sizeof gains, "%s +%d     %s +%d",
                     STAT_NAMES[G.event.primary], G.event.gain_primary,
                     STAT_NAMES[G.event.secondary], G.event.gain_secondary);
        else
            snprintf(gains, sizeof gains, "%s +%d",
                     STAT_NAMES[G.event.primary], G.event.gain_primary);
        rounded_rect(105, 226, 390, 41, 9, 0x17383A, 0.88f * reveal);
        draw_text_center(300, 241, gains, 0xA7E0BC, reveal, 1);
    }
    if (G.event.money_delta) {
        char money[64];
        snprintf(money, sizeof money, "%+d G", G.event.money_delta);
        rounded_rect(214, 226, 172, 41, 9, 0x17383A, 0.88f * reveal);
        draw_text_center(300, 240, money,
                         G.event.money_delta > 0 ? GOLD : CORAL, reveal, 2);
    }
    draw_footer("ENTER  CONTINUE     ESC  SKIP");
}

/* Field-guide entries. The count must equal JOURNAL_PAGES in game.c. */
static const struct { const char *title; const char *body; } JOURNAL_ENTRIES[] = {
    {"THE WEEKLY LIFE",
     "Each choice on the ranch - a drill, a catnap, a shared treat, or a "
     "festival match - moves the calendar one week. Forty-eight weeks make a "
     "year. Plan each season around the rival you mean to face."},
    {"CARE AND REST",
     "Fatigue and stress quietly drain a drill's success and a fighter's edge. "
     "A catnap sheds fatigue; the care basket eases stress, lifts form, and "
     "deepens your bond. A rested Kilix learns faster and fights braver."},
    {"THE SIX GIFTS",
     "Heart, Claw, Flame, Guard, Agility, and Focus shape both training and "
     "battle. Heart is stamina, Claw is power, Flame is spark, Guard softens "
     "hits, Agility dodges, and Focus sharpens every strike."},
    {"DRILL MINI-GAMES",
     "Every drill is a small test of skill, and how well you play sets how "
     "much your Kilix grows. Time the spark, react to the flare, repeat the "
     "sequence, stoke the flame, hold steady, or tap the beat - skill beats luck."},
    {"ARENA TACTICS",
     "Will recharges in real time, so patience pays. Left and right change "
     "range, and each move only works inside its band. Focus lifts accuracy, "
     "Agility slips a blow, and Guard blunts the rest. At the bell, the greater "
     "Heart percentage wins."},
    {"ABOUT THE KILIX",
     "Kilix are fire-kittens born where a kind hearth meets a wild spark. The "
     "tail flame brightens with courage and dims when they need rest. No two "
     "bonds burn the same - raise with patience and let personality lead."},
};
_Static_assert((int)(sizeof JOURNAL_ENTRIES / sizeof JOURNAL_ENTRIES[0])
               == JOURNAL_PAGES, "JOURNAL_ENTRIES must match JOURNAL_PAGES");

static void draw_journal(void)
{
    int sel = clampi(G.journal_page, 0, JOURNAL_PAGES - 1);
    bool book = journal_background.ok;
    uint32_t ink = book ? 0x2E2016 : CREAM;
    uint32_t head = book ? 0x6A2F10 : GOLD;

    if (book)
        draw_bitmap_cover(&journal_background, 0.05f);
    else {
        draw_bitmap_cover(&ranch_background, 0.55f);
        panel(44, 49, 872, 438, 0x17282F, 0.95f);
    }
    /* The book's top spine is dark, so the guide title stays light. */
    draw_text_center(LW / 2, 44, "THE HEARTHKEEPER'S FIELD GUIDE", GOLD, 1.0f, 3);

    /* Left column: the browsable list of entries. */
    for (int i = 0; i < JOURNAL_PAGES; i++) {
        int y = 108 + i * 54;
        bool on = i == sel;
        if (on)
            rounded_rect(66, y - 7, 300, 44, 9,
                         book ? 0x8A4A22 : 0x2E4A50, book ? 0.72f : 0.9f);
        draw_text(90, y + 4, JOURNAL_ENTRIES[i].title,
                  on ? CREAM : (book ? 0x4A3220 : 0x9DB0B8),
                  on ? 1.0f : 0.88f, on ? 2 : 1);
    }

    /* Right column: the selected entry. */
    draw_text(420, 104, JOURNAL_ENTRIES[sel].title, head, 1.0f, 3);
    fill_rect(420, 138, 440, 2, book ? 0x9A6B3A : ORANGE, 0.65f);
    draw_wrapped_text(420, 162, 448, JOURNAL_ENTRIES[sel].body, ink, 0.95f, 2, 6);
    if (sel == JOURNAL_PAGES - 1)          /* the lore page shows the Kilix */
        draw_kilix(660, 495, 235, false, 0);

    char pos[24];
    snprintf(pos, sizeof pos, "%d / %d", sel + 1, JOURNAL_PAGES);
    draw_text_right(884, 44, pos, ink, 0.75f, 1);
    draw_footer("UP / DOWN  BROWSE     ESC  CLOSE");
}

static void draw_champion(void)
{
    draw_bitmap_cover(&arena_background, 0.34f);
    fill_gradient(0, 0, LW, LH, 0x0A1620, 0x5A241A, 0.35f);
    draw_ambient_embers(48, 1.4f);
    draw_kilix(480, 505, 430, false, 1);
    panel(160, 51, 640, 173, NIGHT, 0.94f);
    draw_text_center(480, 79, "CROWN LEAGUE CHAMPION", GOLD, 1.0f, 4);
    char line[96];
    snprintf(line, sizeof line, "%s AND %s LIT THE GRAND HEARTH!",
             "YOU", G.kilix.name);
    draw_text_center(480, 135, line, CREAM, 0.95f, 2);
    snprintf(line, sizeof line, "%d WINS  //  %d WEEKS TO THE CROWN",
             G.kilix.total_wins, G.total_weeks);
    draw_text_center(480, 174, line, 0xB7D7B8, 0.86f, 1);
    draw_footer("ENTER  CONTINUE YOUR RANCH     J  JOURNAL     Q  QUIT");
}

static void draw_save_warning(void)
{
    /* Sticky, on every screen, until the next successful save clears it: a
     * failed autosave must not vanish with the next screen change. */
    if (!G.save_failed) return;
    const char *msg = "SAVE FAILED - CHECK DISK SPACE / PERMISSIONS";
    int width = clampi(text_width(msg, 1) + 40, 260, 760);
    rounded_rect((LW - width) / 2, 8, width, 30, 8, 0x3A1216, 0.94f);
    draw_text_center(LW / 2, 18, msg, CORAL, 0.98f, 1);
}

static void draw_toast_overlay(void)
{
    draw_save_warning();
    if (G.toast_timer <= 0.0f || G.screen == SCREEN_RANCH) return;
    int width = clampi(text_width(G.toast, 1) + 38, 220, 720);
    rounded_rect((LW - width) / 2, 48, width, 38, 10, NIGHT, 0.92f);
    draw_text_center(LW / 2, 61, G.toast, GOLD, 0.97f, 1);
}

static void scale_canvas_to_output(void)
{
    if (!output) return;
    sr_clear(&presented, 0x0a1115u);
    int destination_width = output_width;
    int destination_height = destination_width * LH / LW;
    if (destination_height > output_height) {
        destination_height = output_height;
        destination_width = destination_height * LW / LH;
    }
    int offset_x = (output_width - destination_width) / 2;
    int offset_y = (output_height - destination_height) / 2;
    sr_blit_scaled(&presented, &canvas, offset_x, offset_y,
                   destination_width, destination_height, 1.0f);
    (void)sr_pack_rgba(&presented, output,
                       (size_t)output_width * (size_t)output_height * 4u);
}

bool render_validate_assets(char *error, size_t error_length)
{
    struct {
        const char *path;
        int width;
        int height;
    } expected[] = {
        {"backgrounds/ranch.ppm", 640, 360},
        {"backgrounds/arena.ppm", 640, 360},
        {"kilix.ppm", 384, 384},
        {"kilix_atlas.ppm", 2304, 384},
        {"opponents/mossnub.ppm", 320, 320},
        {"opponents/dewdrop.ppm", 320, 320},
        {"opponents/mistwing.ppm", 320, 320},
        {"opponents/stonecalf.ppm", 320, 320},
        {"opponents/moonmoth.ppm", 320, 320},
        {"opponents/duskcub.ppm", 320, 320}
    };
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        Bitmap bitmap = load_ppm(asset_path(expected[i].path));
        if (!bitmap.ok || bitmap.w != expected[i].width ||
            bitmap.h != expected[i].height) {
            snprintf(error, error_length,
                     "asset %s must be a P6 PPM at %dx%d",
                     expected[i].path, expected[i].width, expected[i].height);
            free_bitmap(&bitmap);
            return false;
        }
        free_bitmap(&bitmap);
    }
    return true;
}

bool render_init(int width, int height, char *error, size_t error_length)
{
    if (!sr_canvas_init(&canvas, LW, LH)) {
        snprintf(error, error_length, "could not allocate logical framebuffer");
        return false;
    }
    ranch_background = load_ppm(asset_path("backgrounds/ranch.ppm"));
    arena_background = load_ppm(asset_path("backgrounds/arena.ppm"));
    kilix_sprite = load_ppm(asset_path("kilix.ppm"));
    if (!ranch_background.ok || ranch_background.w != 640 || ranch_background.h != 360 ||
        !arena_background.ok || arena_background.w != 640 || arena_background.h != 360 ||
        !kilix_sprite.ok || kilix_sprite.w != 384 || kilix_sprite.h != 384) {
        snprintf(error, error_length,
                 "required assets are missing or have unexpected dimensions");
        render_shutdown();
        return false;
    }
    /* Optional at load (draw_kilix/draw_opponent fall back if missing), so a
     * stripped install still runs. */
    kilix_atlas = load_ppm(asset_path("kilix_atlas.ppm"));
    for (int i = 0; i < OPPONENT_COUNT; i++) {
        opponent_sprites[i] = load_ppm(asset_path(OPPONENT_SPRITE_FILES[i]));
        opponent_atlas[i] = load_ppm(asset_path(OPPONENT_ATLAS_FILES[i]));
    }
    for (int i = 0; i < ITEM_COUNT; i++)
        care_sprites[i] = load_ppm(asset_path(CARE_SPRITE_FILES[i]));
    for (int i = 0; i < STAT_COUNT; i++)
        drill_icons[i] = load_ppm(asset_path(DRILL_ICON_FILES[i]));
    journal_background = load_ppm(asset_path("journal.ppm"));
    rent_sprite = load_ppm(asset_path("rent.ppm"));
    mg_board = load_ppm(asset_path("minigame/board.ppm"));
    mg_num[0] = load_ppm(asset_path("minigame/num1.ppm"));
    mg_num[1] = load_ppm(asset_path("minigame/num2.ppm"));
    mg_num[2] = load_ppm(asset_path("minigame/num3.ppm"));
    mg_flame = load_ppm(asset_path("minigame/flame.ppm"));
    mg_bell = load_ppm(asset_path("minigame/bell.ppm"));
    mg_shelter = load_ppm(asset_path("minigame/shelter.ppm"));
    font_atlas = load_ppm(asset_path("font.ppm"));
    render_resize(width, height);
    if (!output) {
        snprintf(error, error_length, "could not allocate terminal framebuffer");
        render_shutdown();
        return false;
    }
    return true;
}

void render_resize(int width, int height)
{
    if (width < 1 || height < 1) return;
    uint8_t *resized = realloc(output, (size_t)width * height * 4);
    if (!resized) return;
    output = resized;
    sr_canvas_free(&presented);
    if (!sr_canvas_init(&presented, width, height)) {
        free(output);
        output = NULL;
        output_width = output_height = 0;
        return;
    }
    output_width = width;
    output_height = height;
    G.W = width;
    G.H = height;
}

void render_shutdown(void)
{
    sr_canvas_free(&canvas);
    sr_canvas_free(&presented);
    free(output);
    output = NULL;
    free_bitmap(&ranch_background);
    free_bitmap(&arena_background);
    free_bitmap(&kilix_sprite);
    free_bitmap(&kilix_atlas);
    for (int i = 0; i < OPPONENT_COUNT; i++) {
        free_bitmap(&opponent_sprites[i]);
        free_bitmap(&opponent_atlas[i]);
    }
    for (int i = 0; i < ITEM_COUNT; i++)
        free_bitmap(&care_sprites[i]);
    for (int i = 0; i < STAT_COUNT; i++)
        free_bitmap(&drill_icons[i]);
    free_bitmap(&journal_background);
    free_bitmap(&rent_sprite);
    free_bitmap(&mg_board);
    for (int i = 0; i < 3; i++)
        free_bitmap(&mg_num[i]);
    free_bitmap(&mg_flame);
    free_bitmap(&mg_bell);
    free_bitmap(&mg_shelter);
    free_bitmap(&font_atlas);
}

/* Short instruction (what to do) and control hint (which keys), kept separate
 * so each renders on one line inside the signboard. */
static const char *drill_action(MinigameType type)
{
    switch (type) {
    case MG_TIMING:   return "STOP THE SPARK IN THE GREEN BAND";
    case MG_REACTION: return "STRIKE WHEN THE EMBER FLARES";
    case MG_MEMORY:   return "WATCH, THEN REPEAT THE SPARKS";
    case MG_MASH:     return "MASH TO STOKE THE FLAME";
    case MG_HOLD:     return "KEEP THE SHELTER STEADY";
    case MG_RHYTHM:   return "TAP EACH BEAT ON TIME";
    default:          return "";
    }
}

static const char *drill_keys(MinigameType type)
{
    switch (type) {
    case MG_TIMING:   return "PRESS SPACE";
    case MG_REACTION: return "PRESS SPACE";
    case MG_MEMORY:   return "KEYS 1 - 4";
    case MG_MASH:     return "MASH SPACE";
    case MG_HOLD:     return "LEFT / RIGHT";
    case MG_RHYTHM:   return "PRESS SPACE";
    default:          return "";
    }
}

/* A fuller, plainer explanation shown on the get-ready screen. */
static const char *drill_detail(MinigameType type)
{
    switch (type) {
    case MG_TIMING:
        return "A spark sweeps the bar three times. Press SPACE to stop it - the "
               "closer to the middle of the green band, the bigger the gain.";
    case MG_REACTION:
        return "Three times, wait for the ember to FLARE, then press SPACE as fast "
               "as you can. Press before it flares and you lose the round.";
    case MG_MEMORY:
        return "The four sparks light up in an order. When they stop, press the "
               "number keys 1-4 to repeat that order. It grows longer each round.";
    case MG_MASH:
        return "Hammer SPACE as fast as you can for a few seconds to fill the "
               "flame gauge. The fuller the gauge, the greater the reward.";
    case MG_HOLD:
        return "The marker drifts on its own. Tap LEFT and RIGHT to keep it inside "
               "the green band. You are scored on how long you stay centered.";
    case MG_RHYTHM:
        return "A heartbeat swells five times at a steady tempo. Tap SPACE right as "
               "the bell peaks - the closer to the beat, the more each tap scores.";
    default:
        return "";
    }
}

/* Make the training Kilix react to how the round is going. */
static int drill_kilix_pose(const MinigameState *m)
{
    if (m->phase == 2)
        return m->quality >= 0.5f ? 1 : 2;          /* happy / worn out */
    if (m->feedback > 0.25f) {
        char c = m->banner[0];
        bool missed = c == 'M' || c == 'T';          /* MISS / TOO... / TIME */
        return missed ? 2 : 1;
    }
    if (m->type == MG_MASH && m->phase == 1)
        return 1;                                    /* bouncing with effort */
    return 0;                                        /* idle-walk */
}

/* Draw a whole sprite centered at (cx,cy) fitting a `size`-box, optional tint. */
static void draw_sprite_at(const Bitmap *b, float cx, float cy, float size,
                           uint32_t tint, float tint_amount)
{
    int s = (int)size;
    if (s <= 0) return;
    draw_bitmap_scaled(b, (int)(cx - s / 2.0f), (int)(cy - s / 2.0f), s, s,
                       false, tint, tint_amount, 1.0f);
}

static void draw_drill(void)
{
    const MinigameState *m = &G.minigame;
    int di = clampi(m->drill, 0, DRILL_COUNT - 1);
    draw_bitmap_cover(&ranch_background, 0.42f);
    fill_gradient(0, 0, LW, LH, 0x101C24, 0x1A1013, 0.28f);
    draw_header(DRILLS[di].name);
    draw_kilix(480, 306, 200, false, drill_kilix_pose(m));

    if (m->phase == 0) {                          /* get-ready: read + countdown */
        int n = clampi(3 - (int)(m->clock / 0.8f), 1, 3);
        /* Wider signboard behind the instructions (procedural panel if absent),
         * sized so every drill's action, control hint, and detail fit inside
         * the parchment without clipping. */
        bool board = mg_board.ok;
        if (board)
            draw_sprite_at(&mg_board, LW / 2, 226, 620, 0xFFFFFF, 0.0f);
        else
            panel(LW / 2 - 250, 56, 500, 300, NIGHT, 0.93f);
        uint32_t htxt = board ? 0x7A2F10 : GOLD;    /* title on the parchment */
        uint32_t atxt = board ? 0x33220F : CREAM;   /* action instruction     */
        uint32_t ktxt = board ? 0x9A5A22 : ORANGE;  /* control-key hint       */
        uint32_t btxt = board ? 0x4A3420 : 0x9DD7BE;
        draw_text_center(LW / 2, 72, DRILLS[di].name, htxt, 0.95f, 3);
        int ay = 116;
        int al = draw_wrapped_center(LW / 2, ay, 452, drill_action(m->type),
                                     atxt, 0.9f, 2, 2);
        int ky = ay + al * 22;
        draw_text_center(LW / 2, ky, drill_keys(m->type), ktxt, 0.92f, 2);
        draw_wrapped_center(LW / 2, ky + 34, 452, drill_detail(m->type),
                            btxt, 0.82f, 1, 4);
        /* Countdown numeral as an animated fire sprite (pops in, settles). */
        float pop = 1.0f - fmodf(m->clock, 0.8f) / 0.8f;         /* 1 -> 0 */
        float scale = 86.0f + pop * 40.0f;
        if (mg_num[n - 1].ok)
            draw_sprite_at(&mg_num[n - 1], LW / 2, 312, scale, 0xFFFFFF, 0.0f);
        else {
            char num[8]; snprintf(num, sizeof num, "%d", n);
            draw_text_center(LW / 2, 300, num, ORANGE, 1.4f, 3);
        }
        return;
    }
    if (m->phase == 2) {                          /* result */
        char pct[32];
        snprintf(pct, sizeof pct, "PERFORMANCE  %d%%",
                 (int)(m->quality * 100.0f + 0.5f));
        panel(LW / 2 - 240, 150, 480, 170, NIGHT, 0.93f);
        draw_text_center(LW / 2, 182, m->banner, GOLD, 1.5f, 3);
        draw_text_center(LW / 2, 224, pct, CREAM, 1.0f, 2);
        draw_progress_bar(LW / 2 - 150, 250, 300, 20, m->quality, 1.0f,
                          0x6AC18D, "");
        draw_text_center(LW / 2, 290, "PRESS ENTER", 0x9DD7BE, 0.9f, 1);
        return;
    }

    /* phase 1: the live game */
    draw_text_center(LW / 2, 66, drill_action(m->type), CREAM, 0.9f, 1);
    if (m->rounds > 1) {
        char rr[48];
        snprintf(rr, sizeof rr, "ROUND %d / %d", m->round + 1, m->rounds);
        draw_text_center(LW / 2, 92, rr, GOLD, 0.85f, 1);
    }
    if (m->feedback > 0.0f)
        draw_text_center(LW / 2, 342, m->banner, ORANGE, 1.3f, 3);

    float glow = 1.0f + sinf(G.time * 8.0f) * 0.08f;   /* gentle idle pulse */
    int bx = LW / 2 - 300, by = 430, bw = 600, bh = 30;
    switch (m->type) {
    case MG_TIMING: {                                  /* flame sweeps the bar */
        rounded_rect(bx, by, bw, bh, 8, 0x101A1F, 0.9f);
        int zx = bx + (int)((m->target - m->half) * bw);
        fill_rect(zx, by + 3, (int)(m->half * 2 * bw), bh - 6, 0x2E7D5B, 0.85f);
        float mx = bx + m->marker * bw;
        if (mg_flame.ok)
            draw_sprite_at(&mg_flame, mx, by - 4, 64 * glow, 0xFFFFFF, 0.0f);
        else
            fill_rect((int)mx - 3, by - 7, 6, bh + 14, GOLD, 1.0f);
        break;
    }
    case MG_REACTION:                                  /* flame flares on cue */
        if (m->cue_live) {
            float f = 190.0f + sinf(G.time * 30.0f) * 18.0f;
            if (mg_flame.ok) draw_sprite_at(&mg_flame, LW / 2, 452, f, 0xFFFFFF, 0.0f);
            else fill_circle(LW / 2, 420, 70, CORAL, 0.8f);
            draw_text_center(LW / 2, 300, "NOW!", 0xFFFFFF, 1.4f, 3);
        } else {
            if (mg_flame.ok)
                draw_sprite_at(&mg_flame, LW / 2, 440, 60, 0x55606A, 0.5f);
            draw_text_center(LW / 2, 300, "WAIT...", 0x9DB0B8, 1.1f, 2);
        }
        break;
    case MG_MEMORY: {                                  /* four glowing spark buttons */
        static const uint32_t cols[4] = {0xF2532E, 0xF6C453, 0x6AC18D, 0x7AA9C9};
        /* During the reveal a symbol lights for the first MEMO_LIT of its slot,
         * then goes dark — so two identical symbols in a row flash separately. */
        int lit = -1;
        if (m->showing) {
            int slot = (int)(m->clock / MEMO_SLOT);
            float within = m->clock - slot * MEMO_SLOT;
            if (slot >= 0 && slot < m->seq_len && within < MEMO_LIT)
                lit = m->seq[slot];
        }
        /* Briefly flash the button the player just matched (feedback set on hit). */
        int pressed = (!m->showing && m->feedback > 0.0f && m->seq_pos > 0)
                      ? m->seq[m->seq_pos - 1] : -1;
        for (int i = 0; i < 4; i++) {
            int cx = LW / 2 - 165 + i * 110;
            bool on = lit == i || pressed == i;
            fill_circle(cx, 430, on ? 46 : 33, cols[i], on ? 1.0f : 0.55f);
            fill_circle(cx, 430, on ? 46 : 33, 0xFFFFFF, on ? 0.28f : 0.0f);
            char lab[16]; snprintf(lab, sizeof lab, "%d", i + 1);
            draw_text_center(cx, 422, lab, INK, 1.0f, 2);
        }
        /* Status line: WATCH while revealing, REPEAT while the player answers. */
        draw_text_center(LW / 2, 388, m->showing ? "WATCH" : "YOUR TURN",
                         m->showing ? 0x9DD7BE : GOLD, 0.9f, 2);
        if (!m->showing)
            for (int i = 0; i < m->seq_len; i++) {
                int cx = LW / 2 - (m->seq_len - 1) * 10 + i * 20;
                fill_circle(cx, 490, 6, i < m->seq_pos ? GOLD : 0x33444A, 1.0f);
            }
        break;
    }
    case MG_MASH: {                                    /* flame grows with taps */
        float frac = clampf((float)m->taps /
                            (m->taps_target > 0 ? m->taps_target : 1), 0, 1);
        char t[16];
        snprintf(t, sizeof t, "%.1fs", clampf(4.0f - m->clock, 0, 4));
        draw_text_center(LW / 2, 470, t, GOLD, 1.0f, 2);
        if (mg_flame.ok) {
            float fs = (80.0f + frac * 210.0f) * (1.0f + sinf(G.time * 18.0f) * 0.05f);
            draw_sprite_at(&mg_flame, LW / 2, 452 - fs * 0.28f, fs, 0xFFFFFF, 0.0f);
        } else {
            draw_progress_bar(bx, by, bw, bh, frac, 1.0f, ORANGE, "FLAME");
        }
        break;
    }
    case MG_HOLD: {                                    /* shelter marker drifts */
        rounded_rect(bx, by, bw, bh, 8, 0x101A1F, 0.9f);
        int zx = bx + (int)((m->target - m->half) * bw);
        fill_rect(zx, by + 3, (int)(m->half * 2 * bw), bh - 6, 0x2E7D5B, 0.8f);
        bool inzone = fabsf(m->marker - m->target) < m->half;
        float mx = bx + m->marker * bw;
        if (mg_shelter.ok)
            draw_sprite_at(&mg_shelter, mx, by - 6, 82, inzone ? 0xFFFFFF : CORAL,
                           inzone ? 0.0f : 0.35f);
        else
            fill_rect((int)mx - 4, by - 7, 8, bh + 14, inzone ? GOLD : CORAL, 1.0f);
        char t[16]; snprintf(t, sizeof t, "%.1fs", clampf(5.0f - m->clock, 0, 5));
        draw_text_center(LW / 2, 470, t, GOLD, 1.0f, 2);
        break;
    }
    case MG_RHYTHM: {                                  /* steady heartbeat to tap */
        /* The bell swells for RHY_APPROACH before each beat and peaks on it, so
         * the beat is anticipated; tap when it is biggest / glowing. */
        float beat = RHY_FIRST + m->round * RHY_PERIOD;
        float dtb = beat - m->clock;                   /* seconds until the beat */
        float swell = 0.0f;                            /* 0 quiet -> 1 on the beat */
        if (dtb >= 0.0f && dtb <= RHY_APPROACH)
            swell = 1.0f - dtb / RHY_APPROACH;
        else if (dtb < 0.0f && dtb > -RHY_WINDOW)
            swell = 1.0f;                              /* hold full across window */
        bool hot = m->cue_live;
        if (hot)                                       /* glow burst on the beat */
            fill_circle(LW / 2, 418, 74 + (int)(sinf(G.time * 40.0f) * 6.0f),
                        0xF6C453, 0.20f);
        float sz = 96.0f + swell * 70.0f;              /* 96 -> 166 at the beat */
        if (mg_bell.ok)
            draw_sprite_at(&mg_bell, LW / 2, 418, sz, 0x6A7078,
                           hot ? 0.0f : 0.42f * (1.0f - swell));
        else
            fill_circle(LW / 2, 418, hot ? 34 : 20 + (int)(swell * 14),
                        hot ? GOLD : 0x33444A, hot ? 1.0f : 0.55f);
        draw_text_center(LW / 2, 300, hot ? "TAP!" : "FEEL THE BEAT",
                         hot ? 0xFFFFFF : 0x9DB0B8, 0.95f, hot ? 3 : 2);
        for (int i = 0; i < m->rounds; i++) {
            int cx = LW / 2 - (m->rounds - 1) * 15 + i * 30;
            fill_circle(cx, 492, 6, i < m->round ? GOLD : 0x33444A, 1.0f);
        }
        break;
    }
    default:
        break;
    }
}

static void draw_academy(void)
{
    draw_bitmap_cover(&ranch_background, 0.06f);
    draw_ambient_embers(14, 0.7f);
    draw_header("ACADEMY - PAID COACHING");

    if (G.academy_phase == 1) {
        /* A short training montage plays before the guaranteed gain lands. */
        float t = clampf(G.academy_clock / ACADEMY_ANIM, 0.0f, 1.0f);
        int poses[4] = {1, 3, 1, 0};
        int pose = poses[(int)(G.academy_clock / 0.28f) % 4];
        draw_kilix(480, 472, 380, false, pose);
        for (int i = 0; i < 10; i++)
            draw_ember(330 + i * 24, 250 + (i & 3) * 20, G.time * 4 + i, 3.0f);
        int stat = clampi(G.academy_choice, 0, STAT_COUNT - 1);
        char cap[64];
        snprintf(cap, sizeof cap, "COACHING %s", STAT_NAMES[stat]);
        panel(300, 78, 360, 110, NIGHT, 0.9f);
        draw_text_center(480, 98, cap, GOLD, 1.0f, 3);
        draw_progress_bar(330, 146, 300, 20, t, 1.0f, 0x6AC18D, "TRAINING...");
        draw_footer("A FOCUSED SESSION IN PROGRESS...");
        return;
    }

    panel(40, 96, 372, 372, NIGHT, 0.9f);
    draw_text(64, 116, "CHOOSE A FOCUS", GOLD, 1.0f, 2);
    char sub[80];
    snprintf(sub, sizeof sub, "%d G   GUARANTEED +%d   ONE WEEK",
             ACADEMY_COST, ACADEMY_GAIN);
    draw_text(65, 142, sub, CREAM, 0.66f, 1);
    for (int s = 0; s < STAT_COUNT; s++) {
        char cur[24];
        snprintf(cur, sizeof cur, "%d", G.kilix.stats.value[s]);
        draw_choice(64, 162 + s * 47, 324, 40, STAT_NAMES[s], cur,
                    G.academy_cursor == s, true);
    }
    draw_kilix(700, 472, 350, false, 1);
    char money[48];
    snprintf(money, sizeof money, "RANCH FUNDS  %d G", G.money);
    draw_text_center(690, 130, money,
                     G.money >= ACADEMY_COST ? GOLD : CORAL, 1.0f, 2);
    draw_footer("ARROWS  CHOOSE     ENTER  TRAIN (-300 G)     ESC  BACK");
}

static void draw_dojo(void)
{
    draw_bitmap_cover(&ranch_background, 0.06f);
    draw_ambient_embers(14, 0.7f);
    draw_header("DOJO - LEARN NEW ATTACKS");

    panel(40, 96, 470, 372, NIGHT, 0.9f);
    draw_text(64, 116, "MOVE SCROLLS", GOLD, 1.0f, 2);
    draw_text(65, 142, "BUY AN ATTACK ONCE; KEEP IT FOR EVERY BATTLE.",
              CREAM, 0.62f, 1);
    for (int i = 0; i < MOVE_COUNT; i++) {
        int y = 160 + i * 66;
        bool sel = G.dojo_cursor == i;
        bool known = (G.moves_known & (1u << i)) != 0;
        rounded_rect(60, y, 428, 58, 9, sel ? ORANGE : NIGHT, sel ? 0.94f : 0.7f);
        if (sel) fill_rect(66, y + 6, 3, 46, GOLD, 0.9f);
        draw_text(80, y + 8, MOVES[i].name, known ? 0x9DDBB4 : CREAM, 1.0f, 2);
        char meta[64];
        snprintf(meta, sizeof meta, "PWR %d   WILL %d   RANGE %02d-%02d",
                 MOVES[i].power, MOVES[i].cost, (int)MOVES[i].min_range,
                 (int)MOVES[i].max_range);
        draw_text(80, y + 34, meta, 0xB9C4B4, 0.7f, 1);
        char tag[32];
        if (known) snprintf(tag, sizeof tag, "LEARNED");
        else snprintf(tag, sizeof tag, "%d G", MOVES[i].price);
        draw_text_right(478, y + 16, tag,
                        known ? 0x9DDBB4
                              : (G.money >= MOVES[i].price ? GOLD : CORAL),
                        1.0f, 2);
    }

    int sel = clampi(G.dojo_cursor, 0, MOVE_COUNT - 1);
    panel(528, 96, 400, 300, NIGHT, 0.9f);
    draw_text(548, 116, MOVES[sel].name, GOLD, 1.0f, 3);
    draw_wrapped_text(548, 154, 360, MOVES[sel].description, CREAM, 0.9f, 1, 4);
    draw_kilix(728, 468, 300, false, 3);
    char money[48];
    snprintf(money, sizeof money, "RANCH FUNDS  %d G", G.money);
    draw_text_center(728, 412, money, GOLD, 1.0f, 2);
    draw_footer("ARROWS  CHOOSE     ENTER  LEARN     ESC  BACK");
}

static void draw_bank(void)
{
    draw_bitmap_cover(&ranch_background, 0.06f);
    draw_ambient_embers(14, 0.7f);
    draw_header("BANK - RENT SAVINGS");

    char line[80];
    panel(40, 96, 430, 158, NIGHT, 0.9f);
    snprintf(line, sizeof line, "COFFERS   %d G", G.money);
    draw_text(64, 116, line, GOLD, 1.0f, 2);
    snprintf(line, sizeof line, "RENT VAULT   %d G", G.bank);
    draw_text(64, 148, line, 0x8FD1B5, 1.0f, 2);

    int weeks_left = G.rent_paid_weeks - G.total_weeks;
    if (weeks_left < 0) weeks_left = 0;
    if (weeks_left == 0)
        snprintf(line, sizeof line, "RENT %d G IS DUE NOW", RENT_AMOUNT);
    else
        snprintf(line, sizeof line, "NEXT RENT  %d G  IN %d WK", RENT_AMOUNT,
                 weeks_left);
    draw_text(64, 194, line, weeks_left <= 1 ? CORAL : CREAM, 0.95f, 1);
    bool vault_covers = G.bank >= RENT_AMOUNT;
    bool covered = G.bank + G.money >= RENT_AMOUNT;
    draw_text(64, 214,
              vault_covers ? "THE VAULT COVERS THE NEXT RENT" :
              covered ? "COFFERS WILL COVER THE SHORTFALL" :
              "WARNING: NEXT RENT CANNOT BE PAID",
              vault_covers ? 0x9DDBB4 : covered ? 0xE0B060 : CORAL, 0.9f, 1);

    static const char *acts[BANK_ACTIONS] = {
        "DEPOSIT 100", "DEPOSIT 500", "DEPOSIT ALL",
        "WITHDRAW 100", "WITHDRAW 500", "WITHDRAW ALL"
    };
    for (int i = 0; i < BANK_ACTIONS; i++)
        draw_choice(64, 268 + i * 33, 406, 28, acts[i], "",
                    G.bank_cursor == i, true);

    if (rent_sprite.ok)
        draw_bitmap_scaled(&rent_sprite, 588, 150, 300, 300, false,
                           0xFFFFFF, 0.0f, 1.0f);
    draw_footer("ARROWS  CHOOSE     ENTER  CONFIRM     ESC  BACK");
}

void render_frame(void)
{
    clear_canvas(NIGHT);
    switch (G.screen) {
    case SCREEN_TITLE: draw_title(); break;
    case SCREEN_NAMING: draw_naming(); break;
    case SCREEN_RANCH: draw_ranch(); break;
    case SCREEN_TRAINING: draw_training(); break;
    case SCREEN_CARE: draw_care(); break;
    case SCREEN_ARENA: draw_arena(); break;
    case SCREEN_BATTLE: draw_battle(); break;
    case SCREEN_DRILL: draw_drill(); break;
    case SCREEN_ACADEMY: draw_academy(); break;
    case SCREEN_DOJO: draw_dojo(); break;
    case SCREEN_BANK: draw_bank(); break;
    case SCREEN_EVENT: draw_event(); break;
    case SCREEN_JOURNAL:
    case SCREEN_CREDITS: draw_journal(); break;
    case SCREEN_CHAMPION: draw_champion(); break;
    default: draw_title(); break;
    }
    draw_toast_overlay();
    float transition = 1.0f - smoothstep(0.0f, 0.28f, G.screen_time);
    if (transition > 0.0f)
        fill_rect(0, 0, LW, LH, NIGHT, transition);
    scale_canvas_to_output();
}

bool render_dump_ppm(const char *path)
{
    return output != NULL && sr_write_ppm(&presented, path);
}
