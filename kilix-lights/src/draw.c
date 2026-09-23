/* draw.c — opaque full-color drawing. Contract: src/draw.h. */
#include "draw.h"

#include "font.h"

static uint8_t channel_mix(uint8_t a, uint8_t b, unsigned t)
{
    return (uint8_t)(((unsigned)a * (255u - t) + (unsigned)b * t + 127u) /
                     255u);
}

static uint32_t color_mix(uint32_t a, uint32_t b, unsigned t)
{
    return ((uint32_t)channel_mix((uint8_t)(a >> 16), (uint8_t)(b >> 16), t)
            << 16) |
           ((uint32_t)channel_mix((uint8_t)(a >> 8), (uint8_t)(b >> 8), t)
            << 8) |
           channel_mix((uint8_t)a, (uint8_t)b, t);
}

void draw_blend_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb,
                     unsigned alpha)
{
    if (c == NULL || c->px == NULL || r < 0) return;
    if (alpha > 255u) alpha = 255u;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            int y = cy + dy;
            if (dx * dx + dy * dy > r * r || x < 0 || y < 0 ||
                x >= c->w || y >= c->h)
                continue;
            size_t i = (size_t)y * (size_t)c->w + (size_t)x;
            c->px[i] = 0xff000000u | color_mix(c->px[i], rgb, alpha);
        }
    }
}

void draw_px(Canvas *c, int x, int y, uint32_t rgb)
{
    if (c == NULL || c->px == NULL) return;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[(size_t)y * (size_t)c->w + (size_t)x] = 0xff000000u | (rgb & 0xffffffu);
}

void draw_clear(Canvas *c, uint32_t rgb)
{
    if (c == NULL || c->px == NULL) return;
    for (int i = 0, n = c->w * c->h; i < n; i++)
        c->px[i] = 0xff000000u | (rgb & 0xffffffu);
}

void draw_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    if (c == NULL || c->px == NULL || w <= 0 || h <= 0) return;
    int x0 = imaxi(0, x), y0 = imaxi(0, y);
    int x1 = imini(c->w, x + w), y1 = imini(c->h, y + h);
    uint32_t v = 0xff000000u | (rgb & 0xffffffu);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (int xx = x0; xx < x1; xx++) row[xx] = v;
    }
}

void draw_frame(Canvas *c, int x, int y, int w, int h, int line, uint32_t rgb)
{
    if (line <= 0 || w <= 0 || h <= 0) return;
    if (line * 2 >= w || line * 2 >= h) {
        draw_rect(c, x, y, w, h, rgb);
        return;
    }
    draw_rect(c, x, y, w, line, rgb);                      /* top    */
    draw_rect(c, x, y + h - line, w, line, rgb);           /* bottom */
    draw_rect(c, x, y + line, line, h - 2 * line, rgb);    /* left   */
    draw_rect(c, x + w - line, y + line, line, h - 2 * line, rgb);
}

void draw_round_rect(Canvas *c, int x, int y, int w, int h, int radius,
                     uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    radius = iclampi(radius, 0, imini(w, h) / 2);
    if (radius == 0) {
        draw_rect(c, x, y, w, h, rgb);
        return;
    }
    draw_rect(c, x + radius, y, w - radius * 2, h, rgb);
    draw_rect(c, x, y + radius, radius, h - radius * 2, rgb);
    draw_rect(c, x + w - radius, y + radius, radius, h - radius * 2, rgb);
    draw_disc(c, x + radius, y + radius, radius, rgb);
    draw_disc(c, x + w - radius - 1, y + radius, radius, rgb);
    draw_disc(c, x + radius, y + h - radius - 1, radius, rgb);
    draw_disc(c, x + w - radius - 1, y + h - radius - 1, radius, rgb);
}

void draw_gradient_v(Canvas *c, int x, int y, int w, int h,
                     uint32_t top, uint32_t bottom)
{
    if (h <= 0) return;
    for (int row = 0; row < h; row++) {
        unsigned t = h == 1 ? 0u : (unsigned)(row * 255 / (h - 1));
        draw_rect(c, x, y + row, w, 1, color_mix(top, bottom, t));
    }
}

void draw_blend_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgb,
                     unsigned alpha)
{
    if (c == NULL || c->px == NULL || w <= 0 || h <= 0) return;
    if (alpha > 255u) alpha = 255u;
    int x0 = imaxi(0, x), y0 = imaxi(0, y);
    int x1 = imini(c->w, x + w), y1 = imini(c->h, y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = 0xff000000u | color_mix(row[xx], rgb, alpha);
    }
}

void draw_dither(Canvas *c, int x, int y, int w, int h, uint32_t a, uint32_t b)
{
    if (c == NULL || c->px == NULL || w <= 0 || h <= 0) return;
    int x0 = imaxi(0, x), y0 = imaxi(0, y);
    int x1 = imini(c->w, x + w), y1 = imini(c->h, y + h);
    uint32_t va = 0xff000000u | (a & 0xffffffu);
    uint32_t vb = 0xff000000u | (b & 0xffffffu);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = (((xx + yy) & 1) == 0) ? va : vb;
    }
}

void draw_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                draw_px(c, cx + dx, cy + dy, rgb);
}

void draw_ring_hard(Canvas *c, int cx, int cy, int r_out, int r_in,
                    uint32_t rgb)
{
    if (r_out < 0) return;
    if (r_in < 0) r_in = 0;
    for (int dy = -r_out; dy <= r_out; dy++)
        for (int dx = -r_out; dx <= r_out; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r_out * r_out && d2 >= r_in * r_in)
                draw_px(c, cx + dx, cy + dy, rgb);
        }
}

void draw_line_hard(Canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    int adx = dx >= 0 ? dx : -dx, ady = dy >= 0 ? dy : -dy;
    int err = adx - ady;

    for (;;) {
        draw_px(c, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = err * 2;
            if (e2 > -ady) { err -= ady; x0 += sx; }
            if (e2 < adx)  { err += adx; y0 += sy; }
        }
    }
}

void draw_shadow(Canvas *c, int x, int y, int w, int h)
{
    draw_blend_rect(c, x + DRAW_SHADOW_OFF, y + DRAW_SHADOW_OFF, w, h,
                    MC_BLACK, 105u);
}

void draw_plate(Canvas *c, int x, int y, int w, int h, uint32_t fill,
                bool shadow)
{
    if (shadow) draw_shadow(c, x, y, w, h);
    draw_rect(c, x, y, w, h, fill);
    draw_frame(c, x, y, w, h, DRAW_BORDER, MC_BLACK);
}

void draw_plate_pressed(Canvas *c, int x, int y, int w, int h)
{
    /* Pressed objects lose their shadow and darken: the object has moved
     * down onto the surface, so nothing is casting. */
    draw_rect(c, x, y, w, h, MC_DARK);
    draw_frame(c, x, y, w, h, DRAW_BORDER, MC_BLACK);
}

/* ---- Text ---- */

int draw_text_width(const char *s)
{
    return font_text_width(s != NULL ? s : "");
}

int draw_text_height(void)
{
    return FONT_CAP_H;
}

void draw_text(Canvas *c, int x, int y, const char *s, uint32_t rgb)
{
    if (c == NULL || c->px == NULL || s == NULL) return;
    font_draw(c, x, y, s, rgb & 0xffffffu);
}

void draw_text_center(Canvas *c, int cx, int y, const char *s, uint32_t rgb)
{
    if (s == NULL) return;
    draw_text(c, cx - draw_text_width(s) / 2, y, s, rgb);
}

/* ---- Cursor ---- */

static const char *const cursor_bits[] = {
    "#       ",
    "##      ",
    "#.#     ",
    "#..#    ",
    "#...#   ",
    "#....#  ",
    "#.....# ",
    "#......#",
    "#...####",
    "#..#    ",
    "#.#     ",
    "##      "
};
enum { CURSOR_H = (int)(sizeof cursor_bits / sizeof cursor_bits[0]) };

void draw_cursor(Canvas *c, int x, int y)
{
    for (int row = 0; row < CURSOR_H; row++) {
        const char *bits = cursor_bits[row];
        for (int col = 0; bits[col] != '\0'; col++) {
            if (bits[col] == '#') draw_px(c, x + col, y + row, MC_BLACK);
            else if (bits[col] == '.') draw_px(c, x + col, y + row, MC_WHITE);
        }
    }
}

bool draw_canvas_opaque(const Canvas *c, int *bad_x, int *bad_y)
{
    if (c == NULL || c->px == NULL) return false;
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            uint32_t pixel = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            if ((pixel >> 24) != 0xffu) {
                if (bad_x) *bad_x = x;
                if (bad_y) *bad_y = y;
                return false;
            }
        }
    }
    return true;
}

size_t draw_chromatic_pixels(const Canvas *c)
{
    size_t count = 0;
    if (c == NULL || c->px == NULL) return 0;
    for (int i = 0, n = c->w * c->h; i < n; i++) {
        uint32_t p = c->px[i];
        unsigned r = (p >> 16) & 0xffu;
        unsigned g = (p >> 8) & 0xffu;
        unsigned b = p & 0xffu;
        unsigned hi = (unsigned)imaxi((int)r, imaxi((int)g, (int)b));
        unsigned lo = (unsigned)imini((int)r, imini((int)g, (int)b));
        if (hi - lo >= 12u) count++;
    }
    return count;
}
