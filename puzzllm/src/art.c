/* art.c - placeholder art; see art.h. */
#include "art.h"
#include "kilix_sprite.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct palette {
    uint32_t floor_a, floor_b, wall, wall_mortar, wall_shadow, runner,
             runner_edge, door_wood, door_dark, frame;
} palette;

static palette room_palette(const char *style)
{
    palette p;
    if (style != NULL && strcmp(style, "brick") == 0) {
        p.floor_a = 0x5a4636; p.floor_b = 0x503e30;
        p.wall = 0x8a4b3a; p.wall_mortar = 0x5e3328; p.wall_shadow = 0x3a201a;
        p.runner = 0x2f5f6f; p.runner_edge = 0xc8a050;
        p.door_wood = 0x6b4a2b; p.door_dark = 0x47301b; p.frame = 0x2a2a30;
    } else {   /* "stone" and anything unknown */
        p.floor_a = 0x3d4250; p.floor_b = 0x363a47;
        p.wall = 0x58606f; p.wall_mortar = 0x3e4451; p.wall_shadow = 0x252831;
        p.runner = 0x7a2f35; p.runner_edge = 0xc8a050;
        p.door_wood = 0x7a5530; p.door_dark = 0x523720; p.frame = 0x1e2026;
    }
    return p;
}

void art_room(sr_canvas *c, const char *style)
{
    palette p = room_palette(style);

    /* floor tiles */
    for (int y = WALL_H; y < CANVAS_H; y += 32)
        for (int x = 0; x < CANVAS_W; x += 32)
            sr_fill_rect(c, (float)x, (float)y, 32, 32,
                         ((x / 32 + y / 32) & 1) ? p.floor_a : p.floor_b, 1.0f);
    /* back wall: staggered blocks */
    sr_fill_rect(c, 0, 0, CANVAS_W, WALL_H, p.wall_mortar, 1.0f);
    for (int row = 0; row * 20 < WALL_H; row++) {
        int off = (row & 1) ? 24 : 0;
        for (int x = -off; x < CANVAS_W; x += 48)
            sr_fill_rect(c, (float)x + 2, (float)row * 20 + 2, 44, 16,
                         sr_scale_rgb(p.wall, 0.9f + 0.1f * (float)((x / 48 + row) % 3) / 2.0f),
                         1.0f);
    }
    sr_fill_rect(c, 0, WALL_H - 6, CANVAS_W, 6, p.wall_shadow, 1.0f);
    sr_fill_rect(c, 0, WALL_H, CANVAS_W, 10, 0x000000, 0.25f);
    /* side walls */
    sr_fill_rect(c, 0, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    sr_fill_rect(c, CANVAS_W - 8, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    /* runner from the door down the aisle to the floor */
    sr_fill_rect(c, AISLE_X0 - 8, WALL_H, (AISLE_X1 - AISLE_X0) + 16,
                 FLOOR_Y - WALL_H + 40, p.runner_edge, 1.0f);
    sr_fill_rect(c, AISLE_X0 - 4, WALL_H, (AISLE_X1 - AISLE_X0) + 8,
                 FLOOR_Y - WALL_H + 36, p.runner, 1.0f);
    /* HUD strip */
    sr_fill_rect(c, 0, HUD_Y, CANVAS_W, CANVAS_H - HUD_Y, 0x000000, 0.55f);
}

void art_door(sr_canvas *c, const char *style, float open, int next_room)
{
    palette p = room_palette(style);
    float lift;
    char sign[32];

    if (open < 0) open = 0;
    if (open > 1) open = 1;
    /* frame */
    sr_fill_rect(c, DOOR_X - 8, DOOR_Y - 8, DOOR_W + 16, DOOR_H + 8, p.frame, 1.0f);
    /* the passage beyond: light spilling from the next room */
    for (int i = 0; i < DOOR_H; i++) {
        float t = (float)i / DOOR_H;
        sr_fill_rect(c, DOOR_X, (float)(DOOR_Y + i), DOOR_W, 1,
                     sr_mix(0xfff1b8, 0xd9a441, t), 1.0f);
    }
    /* door leaf slides up into the wall */
    lift = open * (DOOR_H - 6);
    if (DOOR_H - lift > 0) {
        float top = DOOR_Y;
        float h = DOOR_H - lift;
        sr_fill_rect(c, DOOR_X, top, DOOR_W, h, p.door_wood, 1.0f);
        for (int k = 1; k < 4; k++)
            sr_fill_rect(c, (float)(DOOR_X + k * DOOR_W / 4) - 1, top, 2, h,
                         p.door_dark, 1.0f);
        sr_fill_rect(c, DOOR_X, top + h - 4, DOOR_W, 4, p.door_dark, 1.0f);
        if (open < 0.05f)
            sr_fill_circle(c, DOOR_X + DOOR_W - 12, DOOR_Y + DOOR_H / 2.0f, 4,
                           0xe0c060, 1.0f);
    }
    if (open > 0.0f)   /* light on the runner */
        sr_fill_rect(c, DOOR_X, DOOR_Y + DOOR_H, DOOR_W, 40, 0xfff1b8, 0.25f * open);
    /* lamp above the door: red shut, green open */
    sr_fill_circle(c, DOOR_X + DOOR_W / 2.0f, 6, 4,
                   open >= 1.0f ? 0x40e070 : 0xe04040, 1.0f);
    /* signs either side of the door */
    if (next_room > 0) snprintf(sign, sizeof sign, "ROOM %d", next_room);
    else snprintf(sign, sizeof sign, "SOON");
    sr_fill_rect(c, DOOR_X - 92, 36, 76, 22, 0x1a1a1a, 0.85f);
    sr_text(c, DOOR_X - 86, 39, open >= 1.0f ? "OPEN" : "LOCKED",
            open >= 1.0f ? 0x60f080 : 0xf07060, 1.0f, 1);
    sr_fill_rect(c, DOOR_X + DOOR_W + 16, 36, 76, 22, 0x1a1a1a, 0.85f);
    sr_text(c, DOOR_X + DOOR_W + 22, 39, sign, 0xf0e0b0, 1.0f, 1);
}

void art_screen(sr_canvas *c, rect r, const char *title, bool focused,
                bool near, uint32_t background)
{
    uint32_t bezel = focused ? 0xd8b440 : near ? 0x8090a8 : 0x2c2f38;

    /* shadow, bezel, glass */
    sr_fill_rect(c, (float)r.x + 6, (float)r.y + 8, (float)r.w, (float)r.h, 0x000000, 0.35f);
    sr_fill_rect(c, (float)r.x - 4, (float)r.y - 4, (float)r.w + 8, (float)r.h + 8, bezel, 1.0f);
    sr_fill_rect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 0x15171c, 1.0f);
    sr_fill_rect(c, (float)r.x, (float)r.y, (float)r.w, SCREEN_TITLE_H, 0x262a33, 1.0f);
    sr_text(c, (float)r.x + 8, (float)r.y + 3, title,
            focused ? 0xffe27a : 0xc8ccd6, 1.0f, 1);
    /* power light */
    sr_fill_circle(c, (float)(r.x + r.w - 12), (float)r.y + SCREEN_TITLE_H / 2.0f, 4,
                   focused ? 0x50f080 : 0x2f8f50, 1.0f);
    sr_fill_rect(c, (float)r.x + SCREEN_PAD - 2, (float)(r.y + SCREEN_TITLE_H + SCREEN_PAD - 2),
                 (float)(r.w - 2 * SCREEN_PAD + 4),
                 (float)(r.h - SCREEN_TITLE_H - 2 * SCREEN_PAD + 4), background, 1.0f);
}

void art_kilix(sr_canvas *c, float x, float y, int dir, int step)
{
    const uint32_t *px = kilix_sprite_frame(dir, step);
    int ox = (int)lroundf(x) - KILIX_SPRITE_W / 2;
    int oy = (int)lroundf(y) - KILIX_SPRITE_FOOT;

    sr_fill_ellipse(c, x, y, 18, 6, 0x000000, 0.35f);
    for (int j = 0; j < KILIX_SPRITE_H; j++) {
        int yy = oy + j;
        if (yy < 0 || yy >= c->h) continue;
        for (int i = 0; i < KILIX_SPRITE_W; i++) {
            uint32_t s = px[j * KILIX_SPRITE_W + i];
            uint32_t a = s >> 24;
            int xx = ox + i;
            if (a == 0 || xx < 0 || xx >= c->w) continue;
            if (a == 255) c->px[yy * c->w + xx] = s;
            else sr_blend(c, xx, yy, s & 0xffffff, (float)a / 255.0f);
        }
    }
}

void art_prompt(sr_canvas *c, float x, float y, const char *text)
{
    int w = sr_text_width(text, 1) + 16;
    float bx = x - w / 2.0f;
    if (bx < 8) bx = 8;
    if (bx + w > CANVAS_W - 8) bx = (float)(CANVAS_W - 8 - w);
    sr_fill_rect(c, bx, y - 26, (float)w, 22, 0x101216, 0.9f);
    sr_stroke_rect(c, bx, y - 26, (float)w, 22, 1, 0xd8b440, 1.0f);
    sr_text(c, bx + 8, y - 23, text, 0xfff0c0, 1.0f, 1);
}
