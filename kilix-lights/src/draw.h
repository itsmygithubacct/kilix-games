/* draw.h — full-color primitives and the Parlor visual theme. */
#ifndef KILIX_LIGHTS_DRAW_H
#define KILIX_LIGHTS_DRAW_H

#include "canvas.h"

/* Compatibility names remain for the procedural icon/font code, but these
 * are now warm interface colors rather than a four-grey hardware palette. */
enum {
    MC_BLACK = 0x17212b,
    MC_DARK  = 0x52606d,
    MC_LIGHT = 0xdce6ee,
    MC_WHITE = 0xfffbf4,

    UI_NAVY       = 0x17324d,
    UI_NAVY_LIGHT = 0x285474,
    UI_TEAL       = 0x2a9d8f,
    UI_TEAL_DARK  = 0x19766d,
    UI_GOLD       = 0xe9c46a,
    UI_ORANGE     = 0xf4a261,
    UI_CORAL      = 0xe76f51,
    UI_GREEN      = 0x6f9f62,
    UI_BLUE       = 0x4f86c6,
    UI_PURPLE     = 0x8b6bb0,
    UI_CREAM      = 0xf7eddc,
    UI_WOOD       = 0x84512e,
    UI_WOOD_DARK  = 0x4d2d1b,
    UI_WALL       = 0xd8c8aa,
    UI_SLATE      = 0x34495e
};

void draw_clear(Canvas *c, uint32_t rgb);
void draw_px(Canvas *c, int x, int y, uint32_t rgb);
void draw_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgb);
void draw_frame(Canvas *c, int x, int y, int w, int h, int line,
                uint32_t rgb);
void draw_round_rect(Canvas *c, int x, int y, int w, int h, int radius,
                     uint32_t rgb);
void draw_gradient_v(Canvas *c, int x, int y, int w, int h,
                     uint32_t top, uint32_t bottom);
void draw_blend_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgb,
                     unsigned alpha);
void draw_blend_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb,
                     unsigned alpha);

void draw_dither(Canvas *c, int x, int y, int w, int h, uint32_t a,
                 uint32_t b);
void draw_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb);
void draw_ring_hard(Canvas *c, int cx, int cy, int r_out, int r_in,
                    uint32_t rgb);
void draw_line_hard(Canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb);

enum { DRAW_SHADOW_OFF = 3, DRAW_BORDER = 2 };
void draw_shadow(Canvas *c, int x, int y, int w, int h);
void draw_plate(Canvas *c, int x, int y, int w, int h, uint32_t fill,
                bool shadow);
void draw_plate_pressed(Canvas *c, int x, int y, int w, int h);

void draw_text(Canvas *c, int x, int y, const char *s, uint32_t rgb);
void draw_text_center(Canvas *c, int cx, int y, const char *s, uint32_t rgb);
int  draw_text_width(const char *s);
int  draw_text_height(void);
void draw_cursor(Canvas *c, int x, int y);

/* Presentation guards for the RGB renderer. */
bool   draw_canvas_opaque(const Canvas *c, int *bad_x, int *bad_y);
size_t draw_chromatic_pixels(const Canvas *c);

#endif
