/* art.c - placeholder art; see art.h. */
#include "art.h"
#include "draw.h"
#include "kilix_art.h"

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
            draw_fill(c, (float)x, (float)y, 32, 32,
                         ((x / 32 + y / 32) & 1) ? p.floor_a : p.floor_b, 1.0f);
    /* back wall: staggered blocks */
    draw_fill(c, 0, 0, CANVAS_W, WALL_H, p.wall_mortar, 1.0f);
    for (int row = 0; row * 20 < WALL_H; row++) {
        int off = (row & 1) ? 24 : 0;
        for (int x = -off; x < CANVAS_W; x += 48)
            draw_fill(c, (float)x + 2, (float)row * 20 + 2, 44, 16,
                         sr_scale_rgb(p.wall, 0.9f + 0.1f * (float)((x / 48 + row) % 3) / 2.0f),
                         1.0f);
    }
    draw_fill(c, 0, WALL_H - 6, CANVAS_W, 6, p.wall_shadow, 1.0f);
    draw_fill(c, 0, WALL_H, CANVAS_W, 10, 0x000000, 0.25f);
    /* side walls */
    draw_fill(c, 0, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    draw_fill(c, CANVAS_W - 8, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    /* runner from the door down the aisle to the floor */
    draw_fill(c, AISLE_X0 - 8, WALL_H, (AISLE_X1 - AISLE_X0) + 16,
                 FLOOR_Y - WALL_H + 40, p.runner_edge, 1.0f);
    draw_fill(c, AISLE_X0 - 4, WALL_H, (AISLE_X1 - AISLE_X0) + 8,
                 FLOOR_Y - WALL_H + 36, p.runner, 1.0f);
    /* HUD strip */
    draw_fill(c, 0, HUD_Y, CANVAS_W, CANVAS_H - HUD_Y, 0x000000, 0.55f);
}

void art_door(sr_canvas *c, const char *style, float open, int next_room)
{
    palette p = room_palette(style);
    float lift;
    char sign[32];

    if (open < 0) open = 0;
    if (open > 1) open = 1;
    /* frame */
    draw_fill(c, DOOR_X - 8, DOOR_Y - 8, DOOR_W + 16, DOOR_H + 8, p.frame, 1.0f);
    /* the passage beyond: light spilling from the next room */
    for (int i = 0; i < DOOR_H; i++) {
        float t = (float)i / DOOR_H;
        draw_fill(c, DOOR_X, (float)(DOOR_Y + i), DOOR_W, 1,
                     sr_mix(0xfff1b8, 0xd9a441, t), 1.0f);
    }
    /* door leaf slides up into the wall */
    lift = open * (DOOR_H - 6);
    if (DOOR_H - lift > 0) {
        float top = DOOR_Y;
        float h = DOOR_H - lift;
        draw_fill(c, DOOR_X, top, DOOR_W, h, p.door_wood, 1.0f);
        for (int k = 1; k < 4; k++)
            draw_fill(c, (float)(DOOR_X + k * DOOR_W / 4) - 1, top, 2, h,
                         p.door_dark, 1.0f);
        draw_fill(c, DOOR_X, top + h - 4, DOOR_W, 4, p.door_dark, 1.0f);
        if (open < 0.05f)
            draw_disc(c, DOOR_X + DOOR_W - 12, DOOR_Y + DOOR_H / 2.0f, 4, 4,
                           0xe0c060, 1.0f);
    }
    if (open > 0.0f)   /* light on the runner */
        draw_fill(c, DOOR_X, DOOR_Y + DOOR_H, DOOR_W, 40, 0xfff1b8, 0.25f * open);
    /* lamp above the door: red shut, green open */
    draw_disc(c, DOOR_X + DOOR_W / 2.0f, 6, 4, 4,
                   open >= 1.0f ? 0x40e070 : 0xe04040, 1.0f);
    /* signs either side of the door */
    if (next_room > 0) snprintf(sign, sizeof sign, "ROOM %d", next_room);
    else snprintf(sign, sizeof sign, "SOON");
    draw_fill(c, DOOR_X - 92, 36, 76, 22, 0x1a1a1a, 0.85f);
    draw_text(c, DOOR_X - 86, 39, open >= 1.0f ? "OPEN" : "LOCKED",
            open >= 1.0f ? 0x60f080 : 0xf07060, 1.0f);
    draw_fill(c, DOOR_X + DOOR_W + 16, 36, 76, 22, 0x1a1a1a, 0.85f);
    draw_text(c, DOOR_X + DOOR_W + 22, 39, sign, 0xf0e0b0, 1.0f);
}

void art_screen_glass(sr_canvas *c, rect r, uint32_t background)
{
    /* shadow, glass, title bar and text area: these never change */
    draw_fill(c, (float)r.x + 6, (float)r.y + 8, (float)r.w, (float)r.h, 0x000000, 0.35f);
    draw_fill(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 0x15171c, 1.0f);
    draw_fill(c, (float)r.x, (float)r.y, (float)r.w, SCREEN_TITLE_H, 0x262a33, 1.0f);
    draw_fill(c, (float)r.x + SCREEN_PAD - 2, (float)(r.y + SCREEN_TITLE_H + SCREEN_PAD - 2),
              (float)(r.w - 2 * SCREEN_PAD + 4),
              (float)(r.h - SCREEN_TITLE_H - 2 * SCREEN_PAD + 4), background, 1.0f);
}

void art_screen(sr_canvas *c, rect r, const char *title, bool focused, bool near)
{
    uint32_t bezel = focused ? 0xd8b440 : near ? 0x8090a8 : 0x2c2f38;

    /* the bezel: a ring four pixels wide around the glass */
    draw_fill(c, (float)r.x - 4, (float)r.y - 4, (float)r.w + 8, 4, bezel, 1.0f);
    draw_fill(c, (float)r.x - 4, (float)(r.y + r.h), (float)r.w + 8, 4, bezel, 1.0f);
    draw_fill(c, (float)r.x - 4, (float)r.y, 4, (float)r.h, bezel, 1.0f);
    draw_fill(c, (float)(r.x + r.w), (float)r.y, 4, (float)r.h, bezel, 1.0f);
    draw_text(c, (float)r.x + 8, (float)r.y + 3, title,
              focused ? 0xffe27a : 0xc8ccd6, 1.0f);
    /* power light */
    draw_disc(c, (float)(r.x + r.w - 12), (float)r.y + SCREEN_TITLE_H / 2.0f, 4, 4,
              focused ? 0x50f080 : 0x2f8f50, 1.0f);
}

void art_kilix(sr_canvas *c, float x, float y, int dir, int step_ticks, int idle_ticks)
{
    kilix_pose pose = kilix_pose_at(dir, step_ticks, idle_ticks);
    const kilix_picture *p = kilix_art_get(pose.art);
    int fx = (int)lroundf(x), fy = (int)lroundf(y);

    /* Zell's shadow: on the floor line, under the feet */
    draw_disc(c, x, y, 17.0f, 5.0f, 0x071719, 0.45f);
    if (p == NULL) return;
    draw_picture(c, p->pixels, p->width, p->height, fx - p->width / 2,
                 fy - p->height + pose.offset_y, p->width, p->height, 1.0f);
}

void art_prompt(sr_canvas *c, float x, float y, const char *text)
{
    int w = draw_text_width(text) + 16;
    float bx = x - w / 2.0f;
    if (bx < 8) bx = 8;
    if (bx + w > CANVAS_W - 8) bx = (float)(CANVAS_W - 8 - w);
    draw_fill(c, bx, y - 26, (float)w, 22, 0x101216, 0.9f);
    draw_stroke(c, bx, y - 26, (float)w, 22, 1, 0xd8b440, 1.0f);
    draw_text(c, bx + 8, y - 23, text, 0xfff0c0, 1.0f);
}

void art_chest(sr_canvas *c, int x, int y, bool open)
{
    draw_fill(c, x + 4, y + 28, 56, 7, 0x211817, 0.65f);
    draw_fill(c, x, y + 10, 56, 22, 0x3c241d, 1);
    draw_fill(c, x + 3, y + 12, 50, 17, 0x935428, 1);
    draw_fill(c, x + 2, y + (open ? -8 : 0), 52, 11, 0xbd7837, 1);
    draw_fill(c, x + 8, y + 10, 4, 21, 0xe4ab50, 1);
    draw_fill(c, x + 44, y + 10, 4, 21, 0xe4ab50, 1);
    draw_fill(c, x + 24, y + 8, 8, 8, 0xf0c66a, 1);
    draw_fill(c, x + 27, y + 10, 2, 4, 0x53341e, 1);
    if (open) {
        draw_fill(c, x + 16, y + 3, 9, 8, 0xffdd79, 1);
        draw_fill(c, x + 18, y + 5, 4, 4, 0x53341e, 1);
        draw_fill(c, x + 24, y + 6, 15, 3, 0xffdd79, 1);
        draw_fill(c, x + 35, y + 8, 3, 4, 0xffdd79, 1);
    }
}
