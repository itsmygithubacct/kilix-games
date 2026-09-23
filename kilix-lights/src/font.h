/* font.h — original 7x14 bitmap face with 20px line advance.
 *
 * The face is authored directly for this project from generic bitmap-letterform
 * practice and the written size/legibility requirement alone. It is not
 * extracted, traced, transcribed, or redrawn from any product binary, ROM,
 * screenshot, or historical font. Each glyph is a directly authored seven-row
 * source mask rendered at two vertical pixels per row.
 */
#ifndef KILIX_LIGHTS_FONT_H
#define KILIX_LIGHTS_FONT_H

#include "canvas.h"

enum {
    FONT_GLYPH_W = 7,
    FONT_CAP_H = 14,
    FONT_ADVANCE = 8,
    FONT_LINE_H = 20
};

int  font_text_width(const char *text);
bool font_has_glyph(unsigned char ch);
void font_draw(Canvas *c, int x, int y, const char *text, uint32_t rgb);

#endif
