/* art.h - generated room and alpha-masked switch sprite. */
#ifndef KILIX_LIGHTS_ART_H
#define KILIX_LIGHTS_ART_H

#include "canvas.h"

bool art_init(const char *argv0, bool verbose);
void art_shutdown(void);
bool art_ready(void);
bool art_validate(const char *argv0, bool verbose);

void art_draw_room(Canvas *canvas);
void art_draw_switch(Canvas *canvas, int x, int y, int size,
                     bool on, bool hover, bool focused, bool pressed);
bool art_switch_hit(int x, int y, int size, int px, int py);

#endif
