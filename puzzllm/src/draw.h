/* draw.h - every mark the game puts on the canvas goes through here.
 *
 * Two renderers sit behind these calls:
 *
 *   neuronte  zell-neuronte's renderer: text and terminal glyphs come from
 *             the Neuronte nets, thin bars and outlines are block glyphs,
 *             discs are the black circle, and pictures (Kilix) go through
 *             2 x 4 octant cells whose masks come from the pixel net.
 *   system    Fontconfig's monospace font rendered by FreeType, hand-drawn
 *             box and block characters, and nearest-neighbour pictures.
 *
 * The Neuronte renderer is built when the Makefile finds a Neuronte
 * checkout. It is used when enabled and its weight file opens. The system
 * font is the default, with a built-in bitmap fallback. Wide flat fills are
 * plain code in both, as in Zell.
 *
 * Colours are 0xRRGGBB plus an alpha in 0..1. Text is one 8x16 cell per
 * Unicode scalar.
 */
#ifndef PUZZLLM_DRAW_H
#define PUZZLLM_DRAW_H

#include "soft_raster.h"

#include <stdbool.h>
#include <stdint.h>

enum { DRAW_CELL_W = 8, DRAW_CELL_H = 16 };

/* Open the Neuronte weights when built with Neuronte (once; later calls
 * return the same answer). True when the Neuronte renderer is in use. */
bool draw_init(void);
bool draw_set_neuronte(bool enabled);
bool draw_neuronte_available(void);
bool draw_neuronte_enabled(void);
/* Renderer name, with the weight path when using Neuronte. */
const char *draw_renderer(void);
/* Tests: use the raster renderer even when Neuronte is open. */
void draw_force_raster(bool on);

void draw_fill(sr_canvas *c, float x, float y, float w, float h, uint32_t rgb, float alpha);
void draw_stroke(sr_canvas *c, float x, float y, float w, float h, float thickness,
                 uint32_t rgb, float alpha);
void draw_disc(sr_canvas *c, float cx, float cy, float rx, float ry, uint32_t rgb, float alpha);

/* UTF-8 text, top-left at (x, y). */
void draw_text(sr_canvas *c, float x, float y, const char *s, uint32_t rgb, float alpha);
int draw_text_width(const char *s);

/* One terminal character's ink in a w x h cell at (x, y); the caller has
 * filled its background. */
enum { DRAW_GLYPH_BOLD = 1u, DRAW_GLYPH_ITALIC = 2u };
void draw_glyph(sr_canvas *c, int x, int y, int w, int h, uint32_t scalar,
                uint32_t rgb, unsigned style);
/* 1 when a loaded terminal bundle was trained on this scalar. */
int draw_glyph_trained(uint32_t scalar);

/* A premultiplied-ARGB picture of sw x sh scaled into w x h at (x, y). */
void draw_picture(sr_canvas *c, const uint32_t *px, int sw, int sh,
                  int x, int y, int w, int h, float alpha);

#endif
