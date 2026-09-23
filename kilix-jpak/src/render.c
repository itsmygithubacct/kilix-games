/* Code-native Kilix pixel art rendered through soft-raster. */
#include "kilix_jpak.h"
#include "soft_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f

static sr_canvas logical;
static sr_canvas backdrop;
static sr_canvas screen;
static uint8_t *framebuffer;
static int screen_width, screen_height;
static int offset_x, offset_y;

static const uint32_t theme_primary[10] = {
    0x22d3ee, 0xf59e0b, 0x4ade80, 0xe879f9, 0x60a5fa,
    0xfb7185, 0xa78bfa, 0x2dd4bf, 0xfacc15, 0xf8fafc
};
static const uint32_t theme_secondary[10] = {
    0x7dd3fc, 0xfdba74, 0x86efac, 0xf0abfc, 0x93c5fd,
    0xfda4af, 0xc4b5fd, 0x5eead4, 0xfde68a, 0xcbd5e1
};
static const uint32_t chapter_void_a[10] = {
    0x07101d, 0x160c0a, 0x07140f, 0x100b1b, 0x07121c,
    0x190b08, 0x11081c, 0x061311, 0x171304, 0x10121a
};
static const uint32_t chapter_void_b[10] = {
    0x0b1827, 0x21130d, 0x0b1c15, 0x17102a, 0x0a1d2a,
    0x26100b, 0x1c0d2d, 0x091d18, 0x242007, 0x1b2230
};
static const uint32_t chapter_motif[10] = {
    0x22d3ee, 0xfb923c, 0x4ade80, 0xe879f9, 0x60a5fa,
    0xfb7185, 0xc084fc, 0x2dd4bf, 0xfacc15, 0xe2e8f0
};

static uint32_t vhash(uint32_t value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

static float unit_hash(uint32_t value)
{
    return (vhash(value) >> 8) * (1.0f / 16777216.0f);
}

static void rect(float x, float y, float w, float h, uint32_t color, float alpha)
{
    sr_fill_rect(&logical, x + offset_x, y + offset_y, w, h, color, alpha);
}

static void outline(float x, float y, float w, float h, float line,
                    uint32_t color, float alpha)
{
    sr_stroke_rect(&logical, x + offset_x, y + offset_y, w, h, line, color, alpha);
}

static void circle(float x, float y, float radius, uint32_t color, float alpha)
{
    sr_fill_circle(&logical, x + offset_x, y + offset_y, radius, color, alpha);
}

static void ellipse(float x, float y, float rx, float ry, uint32_t color, float alpha)
{
    sr_fill_ellipse(&logical, x + offset_x, y + offset_y, rx, ry, color, alpha);
}

static void ring(float x, float y, float radius, float width, uint32_t color, float alpha)
{
    sr_ring(&logical, x + offset_x, y + offset_y, radius, width, color, alpha);
}

static void line(float x0, float y0, float x1, float y1, float width,
                 uint32_t color, float alpha)
{
    sr_line(&logical, x0 + offset_x, y0 + offset_y, x1 + offset_x, y1 + offset_y,
            width, color, alpha, 0, 0);
}

static void triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                     uint32_t color, float alpha)
{
    sr_fill_triangle(&logical, x0 + offset_x, y0 + offset_y, x1 + offset_x,
                     y1 + offset_y, x2 + offset_x, y2 + offset_y, color, alpha);
}

static void text(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text(&logical, x + offset_x, y + offset_y, value, color, 1.0f, scale);
}

static void text_shadow(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text_shadow(&logical, x + offset_x, y + offset_y, value, color, 1.0f, scale);
}

static void text_center(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text_center(&logical, x + offset_x, y + offset_y, value, color, 1.0f, scale);
}

static void diamond(float cx, float cy, float rx, float ry, uint32_t color, float alpha)
{
    triangle(cx, cy - ry, cx + rx, cy, cx, cy + ry, color, alpha);
    triangle(cx, cy - ry, cx, cy + ry, cx - rx, cy, color, alpha);
}

static void shape_mark(float cx, float cy, int family, uint32_t color, float alpha)
{
    if (family == 0) ring(cx, cy, 3.0f, 1.2f, color, alpha);
    else if (family == 1) {
        line(cx, cy - 3, cx + 3, cy + 3, 1, color, alpha);
        line(cx + 3, cy + 3, cx - 3, cy + 3, 1, color, alpha);
        line(cx - 3, cy + 3, cx, cy - 3, 1, color, alpha);
    } else outline(cx - 3, cy - 3, 6, 6, 1, color, alpha);
}

static uint32_t family_color(int family)
{
    static const uint32_t colors[3] = {0x22d3ee, 0xe879f9, 0xfbbf24};
    return colors[family < 0 ? 0 : family > 2 ? 2 : family];
}

static void build_backdrop(void)
{
    for (int y = 0; y < LOGICAL_H; y++) {
        float v = (float)y / (LOGICAL_H - 1);
        uint32_t color = v < .55f
            ? sr_mix(0x030712, 0x11112b, v / .55f)
            : sr_mix(0x11112b, 0x240f2b, (v - .55f) / .45f);
        for (int x = 0; x < LOGICAL_W; x++) {
            float edge = fabsf((float)x / (LOGICAL_W - 1) - .5f) * 2.0f;
            backdrop.px[y * LOGICAL_W + x] = 0xff000000u |
                sr_scale_rgb(color, 1.0f - edge * .33f);
        }
    }
    for (int i = 0; i < 180; i++) {
        int x = (int)(unit_hash(1000u + i * 7u) * LOGICAL_W);
        int y = (int)(unit_hash(2000u + i * 11u) * LOGICAL_H);
        uint32_t c = (i % 5 == 0) ? 0x67e8f9 : (i % 7 == 0) ? 0xf0abfc : 0x94a3b8;
        sr_px(&backdrop, x, y, c);
        if (i % 23 == 0) sr_px(&backdrop, x + 1, y, c);
    }
}

bool render_init(int width, int height)
{
    memset(&logical, 0, sizeof logical);
    memset(&backdrop, 0, sizeof backdrop);
    memset(&screen, 0, sizeof screen);
    if (!sr_canvas_init(&logical, LOGICAL_W, LOGICAL_H) ||
        !sr_canvas_init(&backdrop, LOGICAL_W, LOGICAL_H)) {
        render_shutdown(); return false;
    }
    build_backdrop();
    return render_resize(width, height);
}

bool render_resize(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    sr_canvas_free(&screen);
    free(framebuffer); framebuffer = NULL;
    if (!sr_canvas_init(&screen, width, height)) return false;
    framebuffer = malloc((size_t)width * (size_t)height * 4);
    if (!framebuffer) { sr_canvas_free(&screen); return false; }
    screen_width = width; screen_height = height;
    return true;
}

void render_shutdown(void)
{
    sr_canvas_free(&logical);
    sr_canvas_free(&backdrop);
    sr_canvas_free(&screen);
    free(framebuffer); framebuffer = NULL;
    screen_width = screen_height = 0;
}

uint8_t *render_fb(void) { return framebuffer; }

static void draw_void_cell(float x, float y, int tx, int ty)
{
    int theme = G.level_data.theme;
    uint32_t base = ((tx + ty + theme) & 1)
                  ? chapter_void_a[theme] : chapter_void_b[theme];
    uint32_t motif = chapter_motif[theme];
    uint32_t hash = vhash((uint32_t)(G.level + 1) * 0x9e3779b9u ^
                          (uint32_t)(tx * 37 + ty * 149));
    rect(x, y, TILE_SIZE, TILE_SIZE, base, 1);
    if ((hash & 7u) != 0u) return;
    switch (theme) {
    case 0: /* intake conduits */
        line(x + 2, y + 5, x + 11, y + 5, 1, motif, .19f);
        line(x + 11, y + 5, x + 13, y + 8, 1, motif, .19f);
        circle(x + 2, y + 5, 1, motif, .3f);
        break;
    case 1: /* copper pipes and rivets */
        line(x + 3, y + 2, x + 3, y + 13, 2, motif, .13f);
        line(x + 3, y + 3, x + 12, y + 3, 1, motif, .18f);
        circle(x + 12, y + 3, 1, 0xfbbf24, .3f);
        break;
    case 2: /* lichen tendrils */
        line(x + 2, y + 13, x + 7, y + 7, 1, motif, .18f);
        line(x + 7, y + 7, x + 12, y + 9, 1, motif, .16f);
        circle(x + 7, y + 7, 1.4f, motif, .24f);
        break;
    case 3: /* prism facets */
        line(x + 1, y + 13, x + 8, y + 2, 1, motif, .19f);
        line(x + 8, y + 2, x + 14, y + 11, 1, 0x60a5fa, .16f);
        break;
    case 4: /* magnetic field nodes */
        ring(x + 8, y + 8, 5, 1, motif, .13f);
        circle(x + 8, y + 8, 1, 0x67e8f9, .28f);
        break;
    case 5: /* ember vents */
        for (int i = 0; i < 3; i++)
            line(x + 3 + i * 4, y + 4, x + 3 + i * 4, y + 11,
                 1, motif, .16f);
        circle(x + 12, y + 3, 1, 0xfcd34d, .32f);
        break;
    case 6: /* nested lock frames */
        outline(x + 3, y + 3, 10, 10, 1, motif, .13f);
        outline(x + 6, y + 6, 4, 4, 1, 0x67e8f9, .13f);
        break;
    case 7: /* null-garden seed constellations */
        circle(x + 4, y + 11, 1.2f, motif, .26f);
        circle(x + 11, y + 4, .8f, motif, .2f);
        line(x + 4, y + 11, x + 11, y + 4, 1, motif, .12f);
        break;
    case 8: /* crown circuit forks */
        line(x + 8, y + 13, x + 8, y + 7, 1, motif, .2f);
        line(x + 8, y + 7, x + 3, y + 3, 1, motif, .16f);
        line(x + 8, y + 7, x + 13, y + 3, 1, motif, .16f);
        break;
    default: /* core iris spokes */
        ring(x + 8, y + 8, 5, 1, motif, .13f);
        line(x + 8, y + 2, x + 8, y + 14, 1, motif, .12f);
        line(x + 2, y + 8, x + 14, y + 8, 1, motif, .12f);
        break;
    }
}

static void draw_panel(float x, float y, uint32_t base, uint32_t edge, bool carbon)
{
    rect(x, y, 16, 16, sr_scale_rgb(base, .48f), 1);
    rect(x + 1, y + 1, 14, 14, base, 1);
    rect(x + 2, y + 2, 12, 3, sr_scale_rgb(base, 1.34f), .85f);
    line(x + 1, y + 15, x + 15, y + 15, 1, 0x020617, .75f);
    if (carbon) {
        for (int d = -12; d < 16; d += 5)
            line(x + d, y + 14, x + d + 14, y, 1, edge, .18f);
    } else {
        circle(x + 3, y + 12, 1, edge, .55f);
        circle(x + 13, y + 12, 1, edge, .55f);
    }
}

static void draw_tile(int tx, int ty)
{
    int tile = G.level_data.tiles[ty][tx];
    float x = FIELD_X + tx * TILE_SIZE;
    float y = FIELD_Y + ty * TILE_SIZE;
    float pulse = .5f + .5f * sinf(G.scene_time * 4.2f + tx * .6f + ty * .3f);
    uint32_t primary = theme_primary[G.level_data.theme];
    uint32_t secondary = theme_secondary[G.level_data.theme];
    draw_void_cell(x, y, tx, ty);

    switch (tile) {
    case T_EMPTY:
        break;
    case T_PANEL:
        draw_panel(x, y, sr_mix(0x65351b, primary, .28f), secondary, false);
        break;
    case T_PANEL_DARK:
        draw_panel(x, y, sr_mix(0x172033, primary, .18f), secondary, true);
        break;
    case T_STONE:
        rect(x, y, 16, 16, 0x111827, 1);
        rect(x + 1, y + 1, 14, 14, 0x334155, 1);
        line(x + 2, y + 4, x + 13, y + 2, 1, 0x64748b, .65f);
        line(x + 5, y + 4, x + 3, y + 12, 1, 0x0f172a, .8f);
        line(x + 3, y + 12, x + 12, y + 14, 1, 0x0f172a, .8f);
        break;
    case T_LADDER: {
        float shift = fmodf(G.scene_time * 12 + ty * 3, 6.0f);
        line(x + 4, y, x + 4, y + 16, 2, 0x64748b, 1);
        line(x + 12, y, x + 12, y + 16, 2, 0x64748b, 1);
        for (float yy = y - 6 + shift; yy < y + 18; yy += 6)
            line(x + 4, yy, x + 12, yy, 1.5f, 0x22d3ee, .82f);
        if (ty == 0 || G.level_data.tiles[ty - 1][tx] != T_LADDER)
            line(x + 2, y + 1, x + 14, y + 1, 1, 0xa5f3fc, .7f);
        if (ty == FIELD_ROWS - 1 || G.level_data.tiles[ty + 1][tx] != T_LADDER)
            line(x + 2, y + 14, x + 14, y + 14, 1, 0xa5f3fc, .7f);
        break;
    }
    case T_CRYSTAL:
        ring(x + 8, y + 11, 5, 1, primary, .25f + pulse * .2f);
        diamond(x + 8, y + 7, 5, 7, 0xa5f3fc, 1);
        triangle(x + 8, y, x + 8, y + 14, x + 3, y + 7, 0x22d3ee, .65f);
        line(x + 8, y + 1, x + 8, y + 12, 1, 0xffffff, .7f);
        break;
    case T_CRYSTAL_EMPTY:
        ellipse(x + 8, y + 12, 6, 3, 0x1e293b, 1);
        ring(x + 8, y + 11, 4, 1, 0x64748b, .75f);
        break;
    case T_FUEL:
        rect(x + 3, y + 3, 10, 11, 0x0f172a, 1);
        outline(x + 3, y + 3, 10, 11, 1, 0xf8fafc, .75f);
        rect(x + 6, y + 1, 4, 2, 0x94a3b8, 1);
        rect(x + 5, y + 6, 6, 5, 0xf97316, 1);
        line(x + 8, y + 5, x + 8, y + 12, 1, 0xfde68a, .8f);
        break;
    case T_CHARGER:
        ring(x + 8, y + 8, 6, 1.5f, 0x22d3ee, .5f + pulse * .4f);
        ring(x + 8, y + 8, 3 + pulse, 1, 0xa5f3fc, .9f);
        line(x + 8, y + 3, x + 5, y + 9, 2, 0xffffff, .7f);
        line(x + 5, y + 9, x + 10, y + 8, 2, 0xffffff, .7f);
        line(x + 10, y + 8, x + 7, y + 14, 2, 0xffffff, .7f);
        break;
    case T_DRAIN:
        ring(x + 8, y + 8, 6, 2, 0xa855f7, .55f);
        ring(x + 8, y + 8, 4 - pulse, 1, 0xe879f9, .9f);
        circle(x + 8, y + 8, 1.5f, 0x020617, 1);
        for (int i = 0; i < 3; i++) {
            float a = G.scene_time * 2.5f + i * 2.094f;
            circle(x + 8 + cosf(a) * 5, y + 8 + sinf(a) * 5, 1, 0xf0abfc, .8f);
        }
        break;
    case T_COIN:
        circle(x + 8, y + 8, 6, 0x92400e, 1);
        circle(x + 8, y + 8, 4.8f, 0xfacc15, 1);
        ring(x + 8, y + 8, 3, 1, 0xfef3c7, .75f);
        line(x + 8, y + 5, x + 8, y + 11, 1, 0x92400e, .7f);
        break;
    case T_RELIC:
        diamond(x + 8, y + 8, 7, 7, 0x78350f, 1);
        diamond(x + 8, y + 8, 5, 5, 0xf59e0b, 1);
        shape_mark(x + 8, y + 8, (tx + ty) % 3, 0xfffbeb, .9f);
        break;
    case T_LIFE:
        circle(x + 8, y + 9, 6, 0xf97316, 1);
        triangle(x + 3, y + 6, x + 4, y + 1, x + 7, y + 5, 0xf97316, 1);
        triangle(x + 9, y + 5, x + 12, y + 1, x + 13, y + 6, 0xf97316, 1);
        circle(x + 6, y + 8, 1, 0x082f49, 1);
        circle(x + 10, y + 8, 1, 0x082f49, 1);
        line(x + 7, y + 11, x + 9, y + 11, 1, 0xffedd5, 1);
        break;
    case T_STUN:
        circle(x + 8, y + 8, 6, 0x312e81, 1);
        ring(x + 8, y + 8, 5, 1, 0x818cf8, 1);
        line(x + 5, y + 3, x + 9, y + 7, 2, 0xe0e7ff, 1);
        line(x + 9, y + 7, x + 6, y + 13, 2, 0xe0e7ff, 1);
        line(x + 9, y + 7, x + 12, y + 5, 1, 0xe0e7ff, 1);
        break;
    case T_SHIELD:
        ring(x + 8, y + 8, 6, 2, 0x4ade80, .8f);
        triangle(x + 8, y + 3, x + 12, y + 6, x + 8, y + 13, 0x86efac, .5f);
        triangle(x + 8, y + 3, x + 8, y + 13, x + 4, y + 6, 0x22c55e, .5f);
        break;
    case T_SPIKES:
        for (int i = 0; i < 3; i++) {
            float sx = x + 1 + i * 5;
            triangle(sx, y + 14, sx + 3, y + 3 + (i & 1) * 3, sx + 6, y + 14,
                     0xfb7185, 1);
            line(sx + 3, y + 5, sx + 3, y + 12, 1, 0xffe4e6, .7f);
        }
        break;
    case T_ICE:
        draw_panel(x, y, 0x155e75, 0xcffafe, false);
        line(x + 2, y + 3, x + 11, y + 1, 1, 0xecfeff, .85f);
        line(x + 5, y + 7, x + 14, y + 5, 1, 0x67e8f9, .55f);
        break;
    case T_MOSS:
        draw_panel(x, y, 0x365314, 0xbbf7d0, true);
        for (int i = 0; i < 5; i++)
            circle(x + 2 + (i * 7) % 13, y + 2 + (i * 5) % 7, 1.5f,
                   i & 1 ? 0x4ade80 : 0xa3e635, .75f);
        break;
    case T_CONVEYOR_LEFT:
    case T_CONVEYOR_RIGHT: {
        draw_panel(x, y, 0x374151, primary, true);
        int direction = tile == T_CONVEYOR_RIGHT ? 1 : -1;
        float shift = fmodf(G.scene_time * 16, 8.0f);
        for (int i = -1; i < 3; i++) {
            float cx = x + i * 8 + (direction > 0 ? shift : 8 - shift);
            line(cx - direction * 3, y + 5, cx + direction, y + 8, 1.5f, secondary, .9f);
            line(cx + direction, y + 8, cx - direction * 3, y + 11, 1.5f, secondary, .9f);
        }
        break;
    }
    case T_TELEPORT_CYAN: case T_TELEPORT_MAGENTA: case T_TELEPORT_AMBER: {
        int family = tile - T_TELEPORT_CYAN;
        uint32_t color = family_color(family);
        ring(x + 8, y + 8, 6.5f, 1.5f, color, .45f + pulse * .4f);
        ring(x + 8, y + 8, 4.2f, 1, 0xf8fafc, .4f);
        shape_mark(x + 8, y + 8, family, color, 1);
        break;
    }
    case T_SWITCH_CYAN: case T_SWITCH_MAGENTA: case T_SWITCH_AMBER: {
        int family = tile - T_SWITCH_CYAN;
        uint32_t color = family_color(family);
        rect(x + 2, y + 7, 12, 7, 0x1e293b, 1);
        outline(x + 2, y + 7, 12, 7, 1, 0x64748b, 1);
        rect(x + 4, y + 4, 8, 6, color, 1);
        shape_mark(x + 8, y + 7, family, 0xffffff, .9f);
        break;
    }
    case T_BARRIER_CYAN: case T_BARRIER_MAGENTA: case T_BARRIER_AMBER: {
        int family = tile - T_BARRIER_CYAN;
        uint32_t color = family_color(family);
        bool open = G.barriers_open[family];
        rect(x + 1, y, 3, 16, 0x475569, 1);
        rect(x + 12, y, 3, 16, 0x475569, 1);
        if (!open) {
            for (int yy = 2; yy < 16; yy += 4)
                line(x + 4, y + yy, x + 12, y + yy, 2, color, .62f + pulse * .3f);
            shape_mark(x + 8, y + 8, family, 0xffffff, .75f);
        } else {
            circle(x + 2.5f, y + 3, 1, color, .6f);
            circle(x + 13.5f, y + 13, 1, color, .6f);
        }
        break;
    }
    case T_PHASE_GLASS: case T_PHASE_DENSE: case T_PHASE_STEEL: {
        uint16_t time = G.phase_time[ty][tx];
        bool absent = time >= 100 && tile != T_PHASE_STEEL;
        uint32_t color = tile == T_PHASE_STEEL ? 0x94a3b8 :
                         tile == T_PHASE_DENSE ? 0xa855f7 : 0xc084fc;
        if (!absent) {
            rect(x, y, 16, 16, sr_scale_rgb(color, .32f), .88f);
            outline(x + 1, y + 1, 14, 14, 1, color, .8f);
            for (int py = 2; py < 15; py += 4) for (int px = 2; px < 15; px += 4)
                if (((px + py + (int)(G.scene_time * 8)) / 4) & 1)
                    circle(x + px, y + py, .8f, 0xf5d0fe, .7f);
            if (time > 0 && time < 100) {
                float fraction = time / (tile == T_PHASE_GLASS ? 28.0f : 64.0f);
                rect(x, y, 16 * clampf(fraction, 0, 1), 16, 0xf0abfc, .32f);
            }
            if (tile == T_PHASE_STEEL) shape_mark(x + 8, y + 8, 2, 0xffffff, .85f);
        } else {
            for (int i = 0; i < 4; i++)
                circle(x + 3 + i * 3, y + 3 + ((i * 5 + time) % 10), 1, color, .35f);
        }
        break;
    }
    case T_EXIT: {
        uint32_t color = G.exit_open ? 0x4ade80 : 0xfb7185;
        ring(x + 8, y + 8, 7, 2, 0x475569, 1);
        ring(x + 8, y + 8, 5.2f + (G.exit_open ? pulse * .7f : 0), 1.5f, color, .9f);
        if (G.exit_open) circle(x + 8, y + 8, 3.5f, 0x052e16, 1);
        else {
            line(x + 4, y + 4, x + 12, y + 12, 2, color, .8f);
            line(x + 12, y + 4, x + 4, y + 12, 2, color, .8f);
        }
        break;
    }
    default:
        break;
    }
}

static void draw_kilix(float x, float y, float scale, int facing,
                       bool thrust, bool phase, float gait, float animation)
{
    float flip = facing >= 0 ? 1.0f : -1.0f;
    uint32_t orange = 0xf97316, dark = 0x9a3412;
    float wave = sinf(animation);
    float step = wave * gait;
    float bob = fabsf(wave) * .55f * gait;
    float body_y = y - bob * scale;
    /* micro-thruster behind the torso */
    rect(x + (flip > 0 ? -1 : 8) * scale, body_y + 6 * scale, 4 * scale, 7 * scale,
         0x6d28d9, 1);
    rect(x + (flip > 0 ? 0 : 9) * scale, body_y + 7 * scale, 2 * scale, 4 * scale,
         0xa78bfa, 1);
    if (thrust) {
        triangle(x + (flip > 0 ? 0 : 11) * scale, body_y + 12 * scale,
                 x + (flip > 0 ? 2 : 9) * scale, body_y + 12 * scale,
                 x + (flip > 0 ? 1 : 10) * scale,
                 body_y + (16 + sinf(animation * 2) * 1.5f) * scale, 0xfb923c, .95f);
        circle(x + (flip > 0 ? 1 : 10) * scale, body_y + 15 * scale, 1.2f * scale,
               0xfef08a, .9f);
    }
    /* tail remains readable even at one-cell gameplay scale */
    line(x + (flip > 0 ? 4 : 7) * scale, body_y + 10 * scale,
         x + (flip > 0 ? -1 : 12) * scale,
         body_y + (8 + sinf(animation * .72f) * (1.3f + gait)) * scale,
         1.4f * scale, orange, 1);
    ellipse(x + 5.5f * scale, body_y + 9.5f * scale, 4.3f * scale, 5.0f * scale,
            dark, 1);
    rect(x + 2 * scale, body_y + 7 * scale, 7 * scale, 6 * scale, orange, 1);
    /* Arms swing opposite the boots, giving the one-cell sprite a readable
     * gait rather than merely sliding its whole silhouette. */
    line(x + (flip > 0 ? 8 : 3) * scale, body_y + 8 * scale,
         x + (flip > 0 ? 9 + step : 2 - step) * scale,
         body_y + (11 - step * .7f) * scale, 1.5f * scale, 0xfdba74, 1);
    circle(x + 5.5f * scale, body_y + 5.5f * scale, 4.5f * scale, orange, 1);
    triangle(x + 2 * scale, body_y + 4 * scale, x + 2.5f * scale, body_y,
             x + 5 * scale, body_y + 3 * scale, orange, 1);
    triangle(x + 6 * scale, body_y + 3 * scale, x + 9 * scale, body_y,
             x + 9.2f * scale, body_y + 5 * scale, orange, 1);
    triangle(x + 2.7f * scale, body_y + 3.4f * scale,
             x + 3 * scale, body_y + 1.4f * scale,
             x + 4.2f * scale, body_y + 3.1f * scale, 0xfda4af, .9f);
    triangle(x + 7 * scale, body_y + 3.1f * scale,
             x + 8.4f * scale, body_y + 1.2f * scale,
             x + 8.7f * scale, body_y + 3.7f * scale, 0xfda4af, .9f);
    rect(x + (flip > 0 ? 5 : 2.2f) * scale, body_y + 4 * scale,
         4 * scale, 2.4f * scale,
         0x083344, 1);
    rect(x + (flip > 0 ? 6 : 2.7f) * scale, body_y + 4.4f * scale, 2.6f * scale,
         .8f * scale, 0x67e8f9, 1);
    float left_lift = fmaxf(0.0f, step) * 1.5f;
    float right_lift = fmaxf(0.0f, -step) * 1.5f;
    rect(x + (2.2f + step * .75f) * scale,
         body_y + (12 - left_lift) * scale, 3.3f * scale, 2.6f * scale,
         0x22d3ee, 1);
    rect(x + (6.3f - step * .75f) * scale,
         body_y + (12 - right_lift) * scale, 3.3f * scale, 2.6f * scale,
         0x22d3ee, 1);
    if (phase) {
        ring(x + 5.5f * scale, body_y + 7.5f * scale, 7.3f * scale,
             1.0f * scale, 0xe879f9, .48f);
        line(x - scale, body_y + 3 * scale, x + 12 * scale, body_y + 11 * scale,
             .7f * scale, 0xf5d0fe, .35f);
    }
}

static void draw_player(void)
{
    const Player *p = &G.player;
    float x = FIELD_X + p->x;
    float y = FIELD_Y + p->y;
    if (p->invulnerable > 0) {
        float halo = .5f + .5f * sinf(G.scene_time * 3.0f);
        ring(x + 5.5f, y + 7.5f, 8.4f + halo * .8f, 1.0f,
             0xf8fafc, .28f + halo * .28f);
        for (int i = 0; i < 3; i++) {
            float a = G.scene_time * .9f + i * 2.094f;
            circle(x + 5.5f + cosf(a) * 9.5f,
                   y + 7.5f + sinf(a) * 7.5f, .8f, 0xfcd34d, .55f);
        }
    }
    if (p->grounded && p->gait_amount > .05f)
        ellipse(x + 5.5f, y + 14.5f, 5.5f, 1.3f, 0x020617, .38f);
    draw_kilix(x, y, 1.0f, p->facing, p->thrusting, p->phasing,
               p->gait_amount, p->gait_phase + G.scene_time * (1.0f - p->gait_amount));
    if (p->shield > 0) {
        ring(x + 5.5f, y + 7.5f, 9.0f + sinf(G.scene_time * 5) * .6f,
             1.2f, 0x4ade80, .48f);
        ring(x + 5.5f, y + 7.5f, 7.5f, .6f, 0xbbf7d0, .35f);
    }
    if (p->stunner > 0) {
        float a = G.scene_time * 7;
        for (int i = 0; i < 3; i++)
            circle(x + 5.5f + cosf(a + i * 2.094f) * 10,
                   y + 7.5f + sinf(a + i * 2.094f) * 7,
                   1.2f, 0x818cf8, .8f);
    }
}

static void draw_enemy(const Enemy *e, int index)
{
    if (!e->active) return;
    float x = FIELD_X + e->x, y = FIELD_Y + e->y;
    float pulse = .5f + .5f * sinf(e->phase * 3 + index);
    float stun = e->stun > 0 ? .72f : 1.0f;
    /* A shared danger ring keeps every machine identifiable as hostile even
     * when its individual palette resembles a pickup or chapter motif. */
    ring(x + 6, y + 6.5f, 7.2f, .75f, 0xfb7185, .12f + pulse * .08f);
    if (e->alert > 0) {
        float warning = .5f + .5f * sinf(G.scene_time * 6.0f + index);
        ring(x + 6, y + 6.5f, 8.2f + warning, 1.0f, 0xfbbf24,
             .28f + warning * .28f);
    }
    if (e->tell > 0) {
        triangle(x + 3, y - 2, x + 9, y - 2, x + 6, y - 7,
                 0xfbbf24, .9f);
        line(x + 6, y - 6, x + 6, y - 3, 1, 0x451a03, .9f);
    }
    switch (e->kind) {
    case EN_TRACKER:
        rect(x + 1, y + 5, 10, 6, 0x991b1b, stun);
        triangle(x + 2, y + 5, x + 5, y + 1, x + 6, y + 5, 0xfb7185, stun);
        circle(x + (e->vx >= 0 ? 9 : 3), y + 7, 1.5f, 0xfef2f2, stun);
        circle(x + 3, y + 11, 2, 0x334155, stun);
        circle(x + 9, y + 11, 2, 0x334155, stun);
        break;
    case EN_ROLLER:
        circle(x + 6, y + 6, 5.5f, 0x475569, stun);
        ring(x + 6, y + 6, 4, 1.2f, 0xcbd5e1, stun);
        for (int i = 0; i < 3; i++) {
            float a = e->phase * 4 + i * 2.094f;
            circle(x + 6 + cosf(a) * 3.2f, y + 6 + sinf(a) * 3.2f, 1,
                   0x22d3ee, stun);
        }
        break;
    case EN_POGO:
        circle(x + 6, y + 3, 3, 0xa3e635, stun);
        line(x + 6, y + 6, x + 6, y + 10, 2, 0xe2e8f0, stun);
        line(x + 3, y + 7, x + 9, y + 9, 1, 0x84cc16, stun);
        line(x + 6, y + 10, x + 3, y + 12, 1, 0xe2e8f0, stun);
        line(x + 3, y + 12, x + 9, y + 12, 1, 0xe2e8f0, stun);
        break;
    case EN_DART: {
        float angle = atan2f(e->vy, e->vx);
        float dx = cosf(angle), dy = sinf(angle), px = -dy, py = dx;
        triangle(x + 6 + dx * 6, y + 6 + dy * 6,
                 x + 6 - dx * 5 + px * 4, y + 6 - dy * 5 + py * 4,
                 x + 6 - dx * 5 - px * 4, y + 6 - dy * 5 - py * 4,
                 0xf59e0b, stun);
        line(x + 6 - dx * 2, y + 6 - dy * 2, x + 6 + dx * 4,
             y + 6 + dy * 4, 1, 0xfffbeb, stun);
        break;
    }
    case EN_SHARD:
        diamond(x + 6, y + 6, 6, 6, 0xfb7185, stun);
        diamond(x + 6, y + 6, 2.5f, 5, 0xffe4e6, stun);
        break;
    case EN_BLINKER:
        for (int i = 0; i < 6; i++) {
            float a = e->phase * 3 + i * PI / 3;
            circle(x + 6 + cosf(a) * (3.5f + pulse),
                   y + 6 + sinf(a) * (3.5f + pulse), 1.4f,
                   i & 1 ? 0xe879f9 : 0x67e8f9, stun);
        }
        circle(x + 6, y + 6, 2, 0xffffff, stun);
        break;
    case EN_WISP:
        ring(x + 6, y + 6, 5.3f, 1.5f, 0xa78bfa, .55f * stun);
        circle(x + 6, y + 6, 3.8f, 0x312e81, .75f * stun);
        ellipse(x + 6, y + 6, 2.3f, 1.5f, 0xf5d0fe, stun);
        circle(x + 6 + (G.player.x > e->x ? 1 : -1), y + 6, .8f, 0x111827, 1);
        break;
    case EN_WING:
        ellipse(x + 6, y + 7, 3.4f, 4.5f, 0x0f766e, stun);
        triangle(x + 4, y + 6, x - 1, y + 2 + pulse * 2, x + 2, y + 10,
                 0x2dd4bf, .9f * stun);
        triangle(x + 8, y + 6, x + 13, y + 2 + pulse * 2, x + 10, y + 10,
                 0x2dd4bf, .9f * stun);
        circle(x + 5, y + 5, 1, 0xfef08a, stun);
        circle(x + 8, y + 5, 1, 0xfef08a, stun);
        break;
    }
    if (e->stun > 0) {
        ring(x + 6, y + 6, 7.6f, 1.0f, 0x818cf8, .78f);
        line(x + 1, y + 1, x + 4, y - 2, 1.4f, 0xc7d2fe, 1);
        line(x + 7, y, x + 10, y - 2, 1.4f, 0xc7d2fe, 1);
        line(x + 3, y + 13, x + 9, y - 1, .8f, 0x818cf8, .7f);
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &G.particles[i];
        if (!p->active) continue;
        float alpha = clampf(p->life / p->max_life, 0, 1);
        float x = p->x, y = p->y;
        if (G.state != GS_TITLE) { x += FIELD_X; y += FIELD_Y; }
        if (p->kind == PARTICLE_THRUST)
            line(x, y, x - p->vx * .035f, y - p->vy * .035f,
                 p->size, p->color, alpha);
        else if (p->kind == PARTICLE_SHARD)
            diamond(x, y, p->size, p->size * 1.5f, p->color, alpha);
        else circle(x, y, p->size, p->color, alpha);
    }
}

static bool structure_tile(int tile)
{
    return tile == T_PANEL || tile == T_PANEL_DARK || tile == T_STONE ||
           tile == T_ICE || tile == T_MOSS ||
           tile == T_CONVEYOR_LEFT || tile == T_CONVEYOR_RIGHT;
}

static void draw_structure_contours(void)
{
    uint32_t edge = theme_secondary[G.level_data.theme];
    for (int ty = 0; ty < FIELD_ROWS; ty++) for (int tx = 0; tx < FIELD_COLS; tx++) {
        if (!structure_tile(G.level_data.tiles[ty][tx])) continue;
        float x = FIELD_X + tx * TILE_SIZE;
        float y = FIELD_Y + ty * TILE_SIZE;
        bool above = ty > 0 && structure_tile(G.level_data.tiles[ty - 1][tx]);
        bool below = ty + 1 < FIELD_ROWS && structure_tile(G.level_data.tiles[ty + 1][tx]);
        bool left_solid = tx > 0 && structure_tile(G.level_data.tiles[ty][tx - 1]);
        bool right_solid = tx + 1 < FIELD_COLS && structure_tile(G.level_data.tiles[ty][tx + 1]);
        if (!above) line(x + 1, y + 1, x + 15, y + 1, 1, edge, .42f);
        if (!below) line(x + 1, y + 15, x + 15, y + 15, 1, 0x020617, .72f);
        if (!left_solid) line(x + 1, y + 3, x + 1, y + 14, 1, edge, .27f);
        if (!right_solid) line(x + 15, y + 3, x + 15, y + 14, 1, edge, .27f);
    }
}

static void draw_route_anchors(void)
{
    float sx = FIELD_X + G.level_data.spawn_x * TILE_SIZE + 8;
    float sy = FIELD_Y + G.level_data.spawn_y * TILE_SIZE + 8;
    uint32_t primary = theme_primary[G.level_data.theme];
    float spawn_halo = clampf(1.0f - G.level_time / 1.8f, 0.0f, 1.0f);
    ring(sx, sy + 2, 8.5f, 1, 0xf97316, .28f * spawn_halo);
    line(sx - 6, sy + 7, sx + 6, sy + 7, 2, 0xf97316, .8f);
    triangle(sx - 5, sy + 4, sx, sy + 7, sx - 5, sy + 10, 0xfdba74, .75f);
    triangle(sx + 5, sy + 4, sx, sy + 7, sx + 5, sy + 10, 0xfdba74, .75f);

    float ex = FIELD_X + G.level_data.exit_x * TILE_SIZE + 8;
    float ey = FIELD_Y + G.level_data.exit_y * TILE_SIZE + 8;
    ring(ex, ey, 11 + sinf(G.scene_time * 2.5f) * .8f, 1.2f,
         G.exit_open ? 0x4ade80 : 0xfb7185, .28f);
    ring(ex, ey, 14, 1, primary, .14f);
    line(ex, ey - 18, ex, ey - 10, 1, primary, .25f);
    line(ex, ey + 10, ex, ey + 18, 1, primary, .25f);
}

static void draw_hud(void)
{
    offset_x = offset_y = 0;
    uint32_t primary = theme_primary[G.level_data.theme];
    rect(4, 3, 504, 34, 0x050914, .92f);
    outline(4, 3, 504, 34, 1, primary, .48f);
    char buffer[96];
    snprintf(buffer, sizeof buffer, "%03d  %s", G.level + 1, G.level_data.title);
    text_shadow(10, 8, buffer, 0xf8fafc, 1);
    snprintf(buffer, sizeof buffer, "S %07d", G.score);
    text_shadow(426, 8, buffer, 0xfcd34d, 1);
    int awake = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (G.enemies[i].active && G.enemies[i].alert > 0) awake++;
    int elapsed = (int)G.level_time;
    if (elapsed > 99 * 60 + 59) elapsed = 99 * 60 + 59;
    snprintf(buffer, sizeof buffer, "M%02d/%02d  %02d:%02d", awake,
             G.level_data.enemy_count, elapsed / 60, elapsed % 60);
    text(318, 22, buffer, awake ? 0xfb7185 : 0x94a3b8, 1);

    rect(429, 42, 79, 256, 0x050914, .94f);
    outline(429, 42, 79, 256, 1, primary, .5f);
    text_center(468, 49, "KILIX", 0xfdba74, 1);
    line(437, 67, 500, 67, 1, primary, .3f);
    text(438, 73, "FUEL", 0x94a3b8, 1);
    rect(438, 91, 61, 8, 0x111827, 1);
    outline(438, 91, 61, 8, 1, 0x475569, 1);
    float fuel_width = 59 * clampf(G.player.fuel / 100.0f, 0, 1);
    uint32_t fuel_color = G.player.fuel < 18 ? 0xfb7185 :
                          G.player.fuel < 42 ? 0xfbbf24 : 0x22d3ee;
    rect(439, 92, fuel_width, 6, fuel_color, 1);
    snprintf(buffer, sizeof buffer, "%3d%%", (int)G.player.fuel);
    text(450, 103, buffer, 0xe2e8f0, 1);

    int total_motes = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (G.level_data.tiles[y][x] == T_CRYSTAL ||
            G.level_data.tiles[y][x] == T_CRYSTAL_EMPTY) total_motes++;
    int collected = total_motes - G.crystals_remaining;
    text(438, 124, "MOTES", 0x94a3b8, 1);
    snprintf(buffer, sizeof buffer, "%02d", G.crystals_remaining);
    text_shadow(438, 142, buffer, 0x67e8f9, 2);
    snprintf(buffer, sizeof buffer, "/ %02d", total_motes);
    text(472, 151, buffer, 0x64748b, 1);
    for (int i = 0; i < total_motes && i < 8; i++)
        diamond(441 + i * 8, 174, 2.5f, 3.5f,
                i < collected ? 0x22d3ee : 0x334155, 1);

    text(438, 180, "LIVES", 0x94a3b8, 1);
    circle(446, 204, 4, 0xf97316, 1);
    triangle(442, 202, 443, 198, 446, 201, 0xf97316, 1);
    triangle(446, 201, 449, 198, 450, 202, 0xf97316, 1);
    snprintf(buffer, sizeof buffer, "X %d", G.lives);
    text(461, 198, buffer, 0xffedd5, 1);

    bool effect_active = false;
    if (G.player.shield > 0) {
        snprintf(buffer, sizeof buffer, "AEGIS %02d", (int)ceilf(G.player.shield));
        text(438, 220, buffer, 0x4ade80, 1);
        effect_active = true;
    }
    if (G.player.stunner > 0) {
        snprintf(buffer, sizeof buffer, "EMP   %02d", (int)ceilf(G.player.stunner));
        text(438, effect_active ? 239 : 220, buffer, 0x818cf8, 1);
        effect_active = true;
    }
    if (!effect_active) {
        text(438, 220, "STATUS", 0x64748b, 1);
        text(438, 239, "NOMINAL", 0x4ade80, 1);
    }
    line(437, 262, 500, 262, 1, primary, .22f);
    text(438, 266, "IRIS", 0x94a3b8, 1);
    ring(492, 273, 6, 1.5f, G.exit_open ? 0x4ade80 : 0xfb7185, .9f);
    text(438, 280, G.exit_open ? "OPEN" : "LOCKED",
         G.exit_open ? 0x4ade80 : 0xfb7185, 1);
    text(11, 301, "A/D MOVE  W/S RAIL  SPACE JET  X PHASE  H HELP  P PAUSE  M SFX",
         0x94a3b8, 1);
    if (G.banner_timer > 0) {
        float alpha = clampf(G.banner_timer * 2, 0, 1);
        rect(8, 21, 296, 13, 0x020617, .9f * alpha);
        sr_text_center(&logical, 156, 22, G.banner, 0xf8fafc, alpha, 1);
    }
}

static void draw_world(void)
{
    int shake = (int)G.shake;
    offset_x = shake ? (int)(vhash((uint32_t)G.ticks * 3u) %
                                  (uint32_t)(shake * 2 + 1)) - shake : 0;
    offset_y = shake ? (int)(vhash((uint32_t)G.ticks * 7u) %
                                  (uint32_t)(shake * 2 + 1)) - shake : 0;
    sr_canvas_set_clip(&logical, FIELD_X, FIELD_Y, FIELD_W, FIELD_H);
    for (int y = 0; y < FIELD_ROWS; y++)
        for (int x = 0; x < FIELD_COLS; x++) draw_tile(x, y);
    draw_structure_contours();
    draw_route_anchors();
    for (int i = 0; i < MAX_ENEMIES; i++) draw_enemy(&G.enemies[i], i);
    draw_player();
    draw_particles();
    sr_canvas_reset_clip(&logical);
    offset_x = offset_y = 0;
    if (G.flash > 0) {
        sr_canvas_set_clip(&logical, FIELD_X, FIELD_Y, FIELD_W, FIELD_H);
        rect(FIELD_X, FIELD_Y, FIELD_W, FIELD_H,
             G.state == GS_LEVEL_CLEAR ? 0xfef08a : 0xffffff,
             clampf(G.flash, 0, 1) * .27f);
        sr_canvas_reset_clip(&logical);
    }
    outline(FIELD_X - 1, FIELD_Y - 1, FIELD_W + 2, FIELD_H + 2, 1,
            theme_primary[G.level_data.theme], .55f);
    draw_hud();
}

static void panel(float x, float y, float w, float h, uint32_t edge)
{
    rect(x, y, w, h, 0x050914, .94f);
    outline(x, y, w, h, 1, edge, .72f);
    outline(x + 3, y + 3, w - 6, h - 6, 1, 0x334155, .42f);
}

static void draw_title(void)
{
    uint32_t cyan = 0x22d3ee, orange = 0xf97316;
    /* an original starvault iris, orbit, and enlarged Kilix silhouette */
    for (int r = 120; r > 42; r -= 18)
        ring(126, 160, r, 1, r & 2 ? 0x312e81 : 0x164e63, .18f);
    for (int i = 0; i < 14; i++) {
        float a = G.scene_time * (.12f + i * .006f) + i * .73f;
        circle(126 + cosf(a) * (45 + i * 4), 160 + sinf(a) * (28 + i * 2),
               i % 3 == 0 ? 2 : 1, i & 1 ? cyan : 0xe879f9, .45f);
    }
    ring(126, 161, 59 + sinf(G.scene_time) * 2, 4, 0x475569, .8f);
    ring(126, 161, 49, 2, cyan, .5f);
    circle(126, 161, 43, 0x061326, .95f);
    draw_kilix(83, 103, 7.0f, 1, true, false, 0, G.scene_time * 4);

    text_shadow(273, 31, "KILIX", orange, 5);
    text_shadow(291, 102, "JPAK", cyan, 4);
    text(294, 161, "DEEP SALVAGE", 0xf8fafc, 2);
    line(294, 190, 477, 190, 1, cyan, .35f);
    text(307, 194, "A STARVAULT EXPEDITION", 0x94a3b8, 1);

    static const char *const choices[4] = {
        "START CAMPAIGN", "LEVEL SELECT", "FIELD MANUAL", "RETURN TO SHELL"
    };
    panel(270, 211, 222, 80, cyan);
    for (int i = 0; i < 4; i++) {
        float y = 217 + i * 18;
        if (G.menu_choice == i) {
            rect(277, y - 2, 208, 18, 0x0e7490, .42f);
            triangle(282, y + 7, 289, y + 3, 289, y + 11, cyan, 1);
        }
        const char *choice = i == 0 && G.unlocked_level > 0
                           ? "CONTINUE CAMPAIGN" : choices[i];
        text(296, y, choice, G.menu_choice == i ? 0xecfeff : 0x94a3b8, 1);
    }
    panel(22, 264, 222, 28, orange);
    char status[96];
    snprintf(status, sizeof status, "BEST %07d   OPEN %03d/100",
             G.high_score, G.unlocked_level + 1);
    text(31, 270, status, 0xe2e8f0, 1);
    char footer[96];
    snprintf(footer, sizeof footer,
             "ARROWS / W S + ENTER     H HELP     M SOUND %s     Q QUIT",
             G.sound_on ? "ON" : "OFF");
    text(14, 301, footer, 0x94a3b8, 1);
    draw_particles();
}

static void draw_level_preview(const LevelData *preview, float x, float y)
{
    const float cell = 4.0f;
    rect(x - 3, y - 3, FIELD_COLS * cell + 6, FIELD_ROWS * cell + 6,
         0x020617, 1);
    outline(x - 3, y - 3, FIELD_COLS * cell + 6, FIELD_ROWS * cell + 6,
            1, theme_primary[preview->theme], .65f);
    for (int ty = 0; ty < FIELD_ROWS; ty++) for (int tx = 0; tx < FIELD_COLS; tx++) {
        int tile = preview->tiles[ty][tx];
        float px = x + tx * cell, py = y + ty * cell;
        rect(px, py, cell, cell,
             ((tx + ty) & 1) ? chapter_void_a[preview->theme]
                             : chapter_void_b[preview->theme], 1);
        if (tile == T_STONE || tile == T_PHASE_STEEL) {
            rect(px, py, 4, 4, tile == T_STONE ? 0x64748b : 0x475569, .95f);
        } else if (structure_tile(tile)) {
            uint32_t color = tile == T_ICE ? 0x22d3ee : tile == T_MOSS ? 0x65a30d :
                             (tile == T_CONVEYOR_LEFT || tile == T_CONVEYOR_RIGHT)
                             ? 0xfbbf24 : theme_secondary[preview->theme];
            rect(px, py + 2, 4, 2, color, .92f);
            line(px, py + 2, px + 4, py + 2, .7f, 0xe2e8f0, .38f);
        } else if (tile == T_LADDER) {
            rect(px + 1, py, 1, 4, 0x64748b, 1);
            rect(px + 3, py, 1, 4, 0x22d3ee, .9f);
        } else if (tile == T_CRYSTAL) {
            diamond(px + 2, py + 2, 1.6f, 2.0f, 0x67e8f9, 1);
        } else if (tile == T_SPIKES) {
            triangle(px, py + 4, px + 2, py, px + 4, py + 4, 0xfb7185, 1);
        } else if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) {
            rect(px + 1, py, 3, 4, 0xc084fc, .72f);
            line(px, py + 4, px + 4, py, .7f, 0xf5d0fe, .65f);
        } else if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER) {
            rect(px + 1, py, 2, 4, family_color(tile - T_BARRIER_CYAN), 1);
        } else if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER) {
            ring(px + 2, py + 2, 1.7f, .7f,
                 family_color(tile - T_TELEPORT_CYAN), 1);
        } else if (tile >= T_SWITCH_CYAN && tile <= T_SWITCH_AMBER) {
            circle(px + 2, py + 3, 1.2f, family_color(tile - T_SWITCH_CYAN), 1);
        } else if (tile == T_EXIT) {
            ring(px + 2, py + 2, 1.8f, .8f, 0x4ade80, 1);
        } else if (tile != T_EMPTY && tile != T_CRYSTAL_EMPTY) {
            circle(px + 2, py + 2, 1.2f,
                   tile == T_DRAIN || tile == T_STUN ? 0xc084fc :
                   tile == T_CHARGER || tile == T_SHIELD ? 0x4ade80 : 0xf59e0b, 1);
        }
    }
    float sx = x + preview->spawn_x * cell + 2;
    float sy = y + preview->spawn_y * cell + 2;
    int direction = preview->exit_x > preview->spawn_x ? 1 : -1;
    triangle(sx + direction * 2, sy, sx - direction * 1.5f, sy - 1.8f,
             sx - direction * 1.5f, sy + 1.8f, 0xf97316, 1);
    for (int i = 0; i < preview->enemy_count; i++) {
        float ex = x + preview->enemies[i].x * cell + 2;
        float ey = y + preview->enemies[i].y * cell + 2;
        bool airborne = preview->enemies[i].kind == EN_BLINKER ||
                        preview->enemies[i].kind == EN_WISP ||
                        preview->enemies[i].kind == EN_WING;
        if (airborne) diamond(ex, ey, 1.4f, 1.4f, 0xfb7185, 1);
        else rect(ex - 1.5f, ey, 3, 1.5f, 0xfb7185, 1);
    }
}

static void draw_level_select(void)
{
    text_center(256, 14, "STARVAULT NAVIGATION", 0x67e8f9, 2);
    panel(18, 49, 476, 232, 0x22d3ee);
    text(35, 61, "VAULT GRID", 0x94a3b8, 1);
    int selected_row = G.selected_level / 10;
    int selected_col = G.selected_level % 10;
    rect(33, 80 + selected_row * 16, 282, 15,
         theme_primary[selected_row], .08f);
    rect(33 + selected_col * 28, 78, 27, 163,
         theme_primary[selected_row], .055f);
    for (int row = 0; row < 10; row++)
        rect(29, 83 + row * 16, 3, 8, theme_primary[row], .8f);
    for (int row = 0; row < 10; row++) for (int col = 0; col < 10; col++) {
        int level = row * 10 + col;
        float x = 35 + col * 28, y = 82 + row * 16;
        bool unlocked = level <= G.unlocked_level;
        bool selected = level == G.selected_level;
        if (selected) {
            rect(x - 2, y - 2, 27, 15, 0x0e7490, .72f);
            outline(x - 2, y - 2, 27, 15, 1, 0x67e8f9, .75f);
        }
        char number[4]; snprintf(number, sizeof number, "%03d", level + 1);
        text(x, y, number, selected ? 0xffffff : unlocked ? 0x67e8f9 : 0x334155, 1);
    }
    LevelData preview;
    level_build(G.selected_level, &preview);
    draw_level_preview(&preview, 357, 75);
    char chapter[40], stage_name[40];
    const char *slash = strstr(preview.title, " / ");
    if (slash) {
        size_t count = (size_t)(slash - preview.title);
        if (count >= sizeof chapter) count = sizeof chapter - 1;
        memcpy(chapter, preview.title, count); chapter[count] = '\0';
        snprintf(stage_name, sizeof stage_name, "%s", slash + 3);
    } else {
        snprintf(chapter, sizeof chapter, "%s", preview.title);
        stage_name[0] = '\0';
    }
    text(337, 152, chapter, theme_primary[preview.theme], 1);
    text(337, 170, stage_name, 0xf8fafc, 1);
    int motes = 0, hazards = 0, enemy_types = 0;
    bool has_phase = false, has_shutter = false, has_gate = false;
    bool has_ice = false, has_moss = false, has_belts = false;
    unsigned kind_mask = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        int tile = preview.tiles[y][x];
        if (tile == T_CRYSTAL) motes++;
        if (tile == T_SPIKES) hazards++;
        if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) has_phase = true;
        if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER) has_shutter = true;
        if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER) has_gate = true;
        if (tile == T_ICE) has_ice = true;
        if (tile == T_MOSS) has_moss = true;
        if (tile == T_CONVEYOR_LEFT || tile == T_CONVEYOR_RIGHT) has_belts = true;
    }
    for (int i = 0; i < preview.enemy_count; i++)
        kind_mask |= 1u << preview.enemies[i].kind;
    for (int kind = 0; kind < ENEMY_KIND_COUNT; kind++)
        if (kind_mask & (1u << kind)) enemy_types++;
    char info[64];
    snprintf(info, sizeof info, "MOTES %d  MACH %d", motes, preview.enemy_count);
    text(337, 192, info, 0x67e8f9, 1);
    snprintf(info, sizeof info, "HAZ %02d  TYPES %02d", hazards, enemy_types);
    text(337, 210, info, hazards ? 0xfb7185 : 0x94a3b8, 1);
    int pressure = 1 + (preview.enemy_count + enemy_types + hazards * 2 +
                        (has_phase ? 1 : 0) + (has_shutter ? 2 : 0) +
                        (has_gate ? 1 : 0)) / 5;
    if (pressure > 5) pressure = 5;
    snprintf(info, sizeof info, "PRESS %d/5", pressure);
    text(337, 228, info, 0x94a3b8, 1);
    for (int i = 0; i < 5; i++)
        diamond(416 + i * 11, 233, 3, 4, i < pressure ? 0xf59e0b : 0x334155, 1);
    char entry_side = preview.spawn_x < FIELD_COLS / 2 ? 'L' : 'R';
    char iris_side = preview.exit_x < FIELD_COLS / 2 ? 'L' : 'R';
    char horizontal = preview.exit_x > preview.spawn_x ? '>' :
                      preview.exit_x < preview.spawn_x ? '<' : '=';
    int vertical = preview.exit_y - preview.spawn_y;
    const char *vertical_name = vertical < 0 ? "UP" : vertical > 0 ? "DN" : "LV";
    snprintf(info, sizeof info, "ROUTE %c%c%c %s%02d", entry_side, horizontal,
             iris_side, vertical_name, abs(vertical));
    text(337, 246, info, 0xfcd34d, 1);
    char tags[64] = "";
    if (has_phase) strcat(tags, "PH ");
    if (has_shutter) strcat(tags, "SH ");
    if (has_gate) strcat(tags, "GT ");
    if (has_ice) strcat(tags, "IC ");
    if (has_moss) strcat(tags, "LI ");
    if (has_belts) strcat(tags, "BT ");
    if (!tags[0]) strcat(tags, "OPEN");
    text(337, 264, tags, 0x4ade80, 1);
    text_center(256, 289, "LEFT/RIGHT +/-1   UP/DOWN +/-10   ENTER DEPLOY   ESC BACK",
                0x94a3b8, 1);
    text_center(256, 305, "PH PHASE  SH SHUTTER  GT GATE  IC/LI/BT SURFACES",
                0x64748b, 1);
}

static void keycap(float x, float y, const char *label, uint32_t color)
{
    rect(x, y, 34, 30, 0x111827, 1);
    outline(x, y, 34, 30, 2, color, .8f);
    text_center(x + 17, y + 7, label, 0xf8fafc, 1);
}

static void draw_help_object_icon(float x, float y, int tile)
{
    float cx = x + 8, cy = y + 8;
    switch (tile) {
    case T_CRYSTAL:
        ring(cx, cy + 2, 6, 1, 0x22d3ee, .35f);
        diamond(cx, cy, 5, 7, 0xa5f3fc, 1);
        line(cx, cy - 6, cx, cy + 5, 1, 0xffffff, .75f);
        break;
    case T_FUEL:
        rect(x + 3, y + 3, 10, 11, 0x0f172a, 1);
        outline(x + 3, y + 3, 10, 11, 1, 0xf8fafc, .8f);
        rect(x + 6, y + 1, 4, 2, 0x94a3b8, 1);
        rect(x + 5, y + 6, 6, 5, 0xf97316, 1);
        break;
    case T_CHARGER:
        ring(cx, cy, 6, 1.5f, 0x22d3ee, .8f);
        ring(cx, cy, 3, 1, 0xa5f3fc, 1);
        line(cx, cy - 5, cx - 3, cy + 1, 2, 0xffffff, .8f);
        line(cx - 3, cy + 1, cx + 2, cy, 2, 0xffffff, .8f);
        line(cx + 2, cy, cx - 1, cy + 6, 2, 0xffffff, .8f);
        break;
    case T_DRAIN:
        ring(cx, cy, 6, 2, 0xa855f7, .75f);
        ring(cx, cy, 3.5f, 1, 0xe879f9, 1);
        circle(cx, cy, 1.5f, 0x020617, 1);
        break;
    case T_STUN:
        circle(cx, cy, 6, 0x312e81, 1);
        ring(cx, cy, 5, 1, 0x818cf8, 1);
        line(cx - 3, cy - 5, cx + 1, cy - 1, 2, 0xe0e7ff, 1);
        line(cx + 1, cy - 1, cx - 2, cy + 5, 2, 0xe0e7ff, 1);
        line(cx + 1, cy - 1, cx + 4, cy - 3, 1, 0xe0e7ff, 1);
        break;
    case T_SHIELD:
        ring(cx, cy, 6, 2, 0x4ade80, .85f);
        triangle(cx, cy - 5, cx + 4, cy - 2, cx, cy + 5, 0x86efac, .65f);
        triangle(cx, cy - 5, cx, cy + 5, cx - 4, cy - 2, 0x22c55e, .65f);
        break;
    case T_TELEPORT_CYAN:
        ring(cx, cy, 6.5f, 1.5f, family_color(0), .9f);
        ring(cx, cy, 4.2f, 1, 0xf8fafc, .45f);
        shape_mark(cx, cy, 0, family_color(0), 1);
        break;
    case T_SWITCH_CYAN:
        rect(x + 2, y + 7, 12, 7, 0x1e293b, 1);
        outline(x + 2, y + 7, 12, 7, 1, 0x64748b, 1);
        rect(x + 4, y + 4, 8, 6, family_color(0), 1);
        shape_mark(cx, y + 7, 0, 0xffffff, .95f);
        break;
    default:
        outline(x + 2, y + 2, 12, 12, 1, 0x94a3b8, 1);
        break;
    }
}

static void draw_help(void)
{
    static const char *const titles[3] = {"FLIGHT CONTROLS", "SALVAGE PROTOCOL", "THREATS & TERRAIN"};
    text_center(256, 15, titles[G.help_page], 0x67e8f9, 2);
    panel(24, 55, 464, 228, 0x6366f1);
    if (G.help_page == 0) {
        keycap(70, 92, "W", 0x22d3ee); keycap(34, 126, "A", 0x22d3ee);
        keycap(70, 126, "S", 0x22d3ee); keycap(106, 126, "D", 0x22d3ee);
        keycap(170, 108, "SPACE", 0xf97316);
        keycap(242, 108, "X", 0xe879f9);
        draw_kilix(366, 92, 5, 1, false, false, 1, G.scene_time * 7);
        text(42, 177, "A / D", 0x67e8f9, 1); text(108, 177, "WALK", 0xf8fafc, 1);
        text(42, 197, "W / S", 0x67e8f9, 1); text(108, 197, "CLIMB MAGNETIC RAILS", 0xf8fafc, 1);
        text(42, 217, "SPACE / Z", 0xfb923c, 1); text(148, 217, "FIRE MICRO-THRUSTER", 0xf8fafc, 1);
        text(42, 237, "X / E", 0xe879f9, 1); text(108, 237, "ERODE PHASE FOAM", 0xf8fafc, 1);
        text(42, 257, "P / ESC", 0x94a3b8, 1); text(116, 257, "PAUSE", 0xf8fafc, 1);
    } else if (G.help_page == 1) {
        text(43, 75, "RECOVER EVERY STAR MOTE. THE IRIS OPENS ONLY WHEN", 0xf8fafc, 1);
        text(43, 94, "THE DECK IS CLEAN. REACH IT TO DESCEND ONE VAULT.", 0xf8fafc, 1);
        int examples[] = {T_CRYSTAL, T_FUEL, T_CHARGER, T_DRAIN, T_STUN, T_SHIELD,
                          T_TELEPORT_CYAN, T_SWITCH_CYAN};
        const char *labels[] = {"STAR MOTE", "COMET CELL", "CHARGE COIL", "SIPHON",
                                "EMP BELL", "AEGIS", "CIRCLE GATE", "CIRCLE SWITCH"};
        for (int i = 0; i < 8; i++) {
            int col = i & 1, row = i / 2;
            float x = 52 + col * 216, y = 126 + row * 34;
            draw_help_object_icon(x, y, examples[i]);
            text(x + 25, y, labels[i], 0xe2e8f0, 1);
        }
    } else {
        text(43, 74, "EIGHT MACHINE FAMILIES PATROL ACROSS THE CAMPAIGN.", 0xf8fafc, 1);
        text(43, 93, "AEGIS DEFLECTS CONTACT; EMP FREEZES ITS HALO.", 0xf8fafc, 1);
        static const char *const behavior[ENEMY_KIND_COUNT] = {
            "CHASE", "ROLL", "BOUNCE", "CARDINAL",
            "RICOCHET", "WANDER", "HOMING", "FLIGHT"
        };
        for (int i = 0; i < ENEMY_KIND_COUNT; i++) {
            Enemy sample = {.active=true, .kind=i, .x=45 + (i%2)*222,
                            .y=82 + (i/2)*36, .phase=G.scene_time, .vx=20};
            draw_enemy(&sample, i);
            char label[40];
            snprintf(label, sizeof label, "%s  %s", enemy_name(i), behavior[i]);
            text(72 + (i % 2) * 222, 129 + (i / 2) * 36,
                 label, i & 1 ? 0xf0abfc : 0xfda4af, 1);
        }
        text(42, 260, "GLAZE SLIDES  -  LICHEN DRAGS  -  FLUX BELTS PUSH", 0x94a3b8, 1);
    }
    char page[16]; snprintf(page, sizeof page, "%d / 3", G.help_page + 1);
    char footer[96];
    snprintf(footer, sizeof footer, "LEFT / RIGHT PAGE      H OR ESC %s      %s",
             G.help_return_state == GS_TITLE ? "CLOSE" : "RESUME", page);
    text_center(256, 297, footer, 0x94a3b8, 1);
}

static void overlay_box(const char *title, const char *subtitle, uint32_t color)
{
    rect(0, 0, LOGICAL_W, LOGICAL_H, 0x020617, .58f);
    panel(104, 104, 304, 112, color);
    text_center(256, 123, title, color, 2);
    text_center(256, 165, subtitle, 0xe2e8f0, 1);
}

static void draw_state_overlay(void)
{
    char buffer[128];
    if (G.state == GS_PAUSED) {
        rect(0, 0, LOGICAL_W, LOGICAL_H, 0x020617, .62f);
        panel(104, 70, 304, 180, 0x67e8f9);
        text_center(256, 88, "PAUSED", 0x67e8f9, 2);
        line(128, 126, 384, 126, 1, 0x22d3ee, .28f);
        text(138, 138, "P / ESC", 0x67e8f9, 1);
        text(245, 138, "RESUME FLIGHT", 0xf8fafc, 1);
        text(138, 163, "H", 0x818cf8, 1);
        text(245, 163, "FIELD MANUAL", 0xf8fafc, 1);
        text(138, 188, "R", 0xfcd34d, 1);
        text(245, 188, "SPEND LIFE / RESTART", 0xf8fafc, 1);
        text(138, 213, "Q", 0xfb7185, 1);
        text(245, 213, "RETURN TO TITLE", 0xf8fafc, 1);
    } else if (G.state == GS_LEVEL_CLEAR) {
        rect(0, 0, LOGICAL_W, LOGICAL_H, 0x020617, .58f);
        panel(88, 90, 336, 142, 0x4ade80);
        text_center(256, 107, "VAULT SECURED", 0x4ade80, 2);
        snprintf(buffer, sizeof buffer, "BASE +0500   TIME +%04d", G.clear_time_bonus);
        text_center(256, 148, buffer, 0xe2e8f0, 1);
        snprintf(buffer, sizeof buffer, "FUEL +%04d   TOTAL %07d",
                 G.clear_fuel_bonus, G.score);
        text_center(256, 169, buffer, 0xfcd34d, 1);
        text_center(256, 204, "ENTER TO DESCEND", 0x94a3b8, 1);
    } else if (G.state == GS_LIFE_LOST) {
        snprintf(buffer, sizeof buffer, "%d LIVES REMAIN", G.lives);
        overlay_box("SIGNAL LOST", buffer, 0xfb7185);
        char cause[80];
        snprintf(cause, sizeof cause, "CAUSE: %s",
                 G.death_reason[0] ? G.death_reason : "UNKNOWN");
        text_center(256, 187, cause, 0xfda4af, 1);
        text_center(256, 202, "REDEPLOYING...", 0x94a3b8, 1);
    } else if (G.state == GS_GAMEOVER) {
        rect(0, 0, LOGICAL_W, LOGICAL_H, 0x02030a, .88f);
        text_center(256, 65, "EXPEDITION ENDED", 0xfb7185, 3);
        snprintf(buffer, sizeof buffer, "FINAL SCORE  %07d", G.score);
        text_center(256, 145, buffer, 0xfcd34d, 2);
        snprintf(buffer, sizeof buffer, "DEEPEST VAULT  %03d", G.level + 1);
        text_center(256, 188, buffer, 0xcbd5e1, 1);
        text_center(256, 244, "ENTER TO RETURN", 0x94a3b8, 1);
    }
}

static void draw_victory(void)
{
    for (int i = 0; i < 70; i++) {
        float a = i * 2.39996f + G.scene_time * .08f;
        float r = 12 + i * 3.3f;
        circle(256 + cosf(a) * r, 145 + sinf(a) * r * .55f,
               i % 9 == 0 ? 2 : 1, i & 1 ? 0x67e8f9 : 0xfcd34d, .65f);
    }
    ring(256, 145, 75 + sinf(G.scene_time) * 3, 5, 0x22d3ee, .55f);
    ring(256, 145, 58, 2, 0xfcd34d, .8f);
    circle(256, 145, 48, 0x052e16, .9f);
    draw_kilix(223, 97, 6.0f, 1, true, false, 0, G.scene_time * 5);
    text_center(256, 25, "STARVAULT RESTORED", 0xfcd34d, 3);
    text_center(256, 246, "ALL 100 VAULTS SECURED", 0x67e8f9, 2);
    char buffer[64]; snprintf(buffer, sizeof buffer, "FINAL SCORE %07d", G.score);
    text_center(256, 282, buffer, 0xf8fafc, 1);
    text_center(256, 302, "ENTER TO RETURN TO THE STARS", 0x94a3b8, 1);
}

void render_frame(void)
{
    offset_x = offset_y = 0;
    sr_blit(&logical, &backdrop, 0, 0);
    if (G.state == GS_TITLE) draw_title();
    else if (G.state == GS_LEVEL_SELECT) draw_level_select();
    else if (G.state == GS_HELP) draw_help();
    else if (G.state == GS_VICTORY) draw_victory();
    else {
        draw_world();
        draw_state_overlay();
    }
    sr_scale_canvas(&screen, &logical);
    (void)sr_pack_rgba(&screen, framebuffer,
                       (size_t)screen_width * (size_t)screen_height * 4);
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&screen, path);
}
