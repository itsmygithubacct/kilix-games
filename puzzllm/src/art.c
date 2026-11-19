/* art.c - room materials, props, and screen bezels. */
#include "art.h"
#include "draw.h"
#include "kilix_art.h"
#include "kilix_assets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static const art_sprite *room_wall(const room_art *art, const char *style)
{
    if (!art) return NULL;
    return style && strcmp(style, "brick") == 0 ? &art->brick : &art->stone;
}

static void draw_wall_fallback(sr_canvas *c, palette p)
{
    draw_fill(c, 0, 0, CANVAS_W, WALL_H, p.wall_mortar, 1.0f);
    for (int row = 0; row * 20 < WALL_H; row++) {
        int off = (row & 1) ? 24 : 0;
        for (int x = -off; x < CANVAS_W; x += 48)
            draw_fill(c, (float)x + 2, (float)row * 20 + 2, 44, 16,
                         sr_scale_rgb(p.wall, 0.9f + 0.1f * (float)((x / 48 + row) % 3) / 2.0f),
                         1.0f);
    }
}

void art_room(sr_canvas *c, const room_art *art, const char *style)
{
    palette p = room_palette(style);
    const art_sprite *wall = room_wall(art, style);

    if (wall && wall->pixels) {
        for (int x = 0; x < CANVAS_W; x += wall->w)
            draw_picture(c, wall->pixels, wall->w, wall->h, x, 0, wall->w, WALL_H, 1);
    } else {
        draw_wall_fallback(c, p);
    }
    if (art && art->floor.pixels) {
        const art_sprite *floor = &art->floor;
        for (int y = WALL_H; y < CANVAS_H; y += floor->h)
            for (int x = 0; x < CANVAS_W; x += floor->w)
                draw_picture(c, floor->pixels, floor->w, floor->h,
                             x, y, floor->w, floor->h, 1);
    } else {
        for (int y = WALL_H; y < CANVAS_H; y += 32)
            for (int x = 0; x < CANVAS_W; x += 32)
                draw_fill(c, x, y, 32, 32,
                          ((x / 32 + y / 32) & 1) ? p.floor_a : p.floor_b, 1);
    }
    draw_fill(c, 0, WALL_H - 6, CANVAS_W, 6, p.wall_shadow, 1.0f);
    draw_fill(c, 0, WALL_H, CANVAS_W, 10, 0x000000, 0.25f);
    /* side walls */
    draw_fill(c, 0, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    draw_fill(c, CANVAS_W - 8, 0, 8, CANVAS_H, p.wall_shadow, 1.0f);
    /* runner from the door down the aisle to the floor */
    draw_fill(c, AISLE_X0 - 8, WALL_H, (AISLE_X1 - AISLE_X0) + 16,
                 FLOOR_Y - WALL_H + 40, p.runner_edge, 1.0f);
    int runner_h = FLOOR_Y - WALL_H + 36;
    draw_fill(c, AISLE_X0 - 5, WALL_H, AISLE_X1 - AISLE_X0 + 10,
              runner_h + 2, sr_scale_rgb(p.runner, 0.58f), 1);
    draw_fill(c, AISLE_X0 - 2, WALL_H, AISLE_X1 - AISLE_X0 + 4,
              runner_h, p.runner, 1);
    for (int y = WALL_H + 24; y < WALL_H + runner_h; y += 24)
        draw_fill(c, AISLE_X0 - 2, y, AISLE_X1 - AISLE_X0 + 4, 1, 0x000000, 0.12f);
    /* HUD strip */
    draw_fill(c, 0, HUD_Y, CANVAS_W, CANVAS_H - HUD_Y, 0x000000, 0.55f);
}

/* A simplified view down the next room's aisle at doorway scale. */
static void draw_next_room(sr_canvas *c, const room_art *art, const char *style)
{
    int x = DOOR_X, y = DOOR_Y;
    palette p = room_palette(style);
    const art_sprite *wall = room_wall(art, style);

    draw_fill(c, x, y, DOOR_W, 28, p.wall, 1);
    if (wall && wall->pixels)
        draw_picture(c, wall->pixels, wall->w, wall->h, x, y, DOOR_W, 28, 1);
    draw_fill(c, x, y + 28, DOOR_W, DOOR_H - 28, p.floor_a, 1);
    if (art && art->floor.pixels) {
        const art_sprite *floor = &art->floor;
        draw_picture(c, floor->pixels, floor->w, floor->h,
                     x, y + 28, DOOR_W, DOOR_H - 28, 1);
    }
    /* Back door and runner align with the room's near entrance. */
    draw_fill(c, x + 32, y + 12, 16, 17, p.wall_shadow, 1);
    draw_fill(c, x + 35, y + 14, 10, 14, p.door_wood, 1);
    draw_fill(c, x + 31, y + 28, 18, DOOR_H - 28, p.runner_edge, 1);
    draw_fill(c, x + 33, y + 28, 14, DOOR_H - 28, p.runner, 1);

    for (int side = 0; side < 2; side++) {
        int px = x + (side ? 52 : 8);
        draw_fill(c, px, y + 33, 20, 24, 0x292e36, 1);
        draw_fill(c, px + 2, y + 39, 16, 16, 0x101318, 1);
        draw_fill(c, px + 15, y + 35, 2, 2, 0x4a9b73, 1);
    }
    draw_fill(c, x, y, DOOR_W, DOOR_H, 0x080b13, 0.17f);
    draw_fill(c, x, y, 4, DOOR_H, 0x121419, 1);
    draw_fill(c, x + DOOR_W - 4, y, 4, DOOR_H, 0x121419, 1);
}

void art_door(sr_canvas *c, const room_art *art, const char *style, float open,
              int next_room, const char *next_style)
{
    palette p = room_palette(style);
    float lift;
    char sign[32];

    if (open < 0) open = 0;
    if (open > 1) open = 1;
    /* frame */
    draw_fill(c, DOOR_X - 8, DOOR_Y - 8, DOOR_W + 16, DOOR_H + 8, p.frame, 1.0f);
    if (open > 0 && next_room > 0)
        draw_next_room(c, art, next_style);
    else
        draw_fill(c, DOOR_X, DOOR_Y, DOOR_W, DOOR_H, p.wall_shadow, 1);
    /* door leaf slides up into the wall */
    lift = open * (DOOR_H - 6);
    int h = DOOR_H - (int)lroundf(lift);
    if (art && art->door.pixels) {
        const art_sprite *door = &art->door;
        int rows = (door->h * h + DOOR_H - 1) / DOOR_H;
        const uint32_t *bottom = door->pixels + (door->h - rows) * door->w;
        draw_fill(c, DOOR_X, DOOR_Y, DOOR_W, h, p.door_dark, 1);
        draw_picture(c, bottom, door->w, rows, DOOR_X, DOOR_Y, DOOR_W, h, 1);
    } else {
        float top = DOOR_Y;
        draw_fill(c, DOOR_X, top, DOOR_W, h, p.door_wood, 1.0f);
        for (int k = 1; k < 4; k++)
            draw_fill(c, (float)(DOOR_X + k * DOOR_W / 4) - 1, top, 2, h,
                         p.door_dark, 1.0f);
        draw_fill(c, DOOR_X, top + h - 4, DOOR_W, 4, p.door_dark, 1.0f);
        if (open < 0.05f)
            draw_disc(c, DOOR_X + DOOR_W - 12, DOOR_Y + DOOR_H / 2.0f, 4, 4,
                           0xe0c060, 1.0f);
    }
    /* lamp above the door: red shut, green open */
    draw_disc(c, DOOR_X + DOOR_W / 2.0f, 6, 4, 4,
                   open >= 1.0f ? 0x40e070 : 0xe04040, 1.0f);
    int sign_x = DOOR_X + DOOR_W + 16;
    if (next_room > 0) snprintf(sign, sizeof sign, "ROOM %d", next_room);
    else snprintf(sign, sizeof sign, "SOON");
    draw_fill(c, sign_x, 18, 76, 43, 0x1a1a1a, 0.85f);
    draw_text(c, sign_x + 6, 22, sign, 0xf0e0b0, 1.0f);
    draw_text(c, sign_x + 6, 42, open >= 1.0f ? "OPEN" : "LOCKED",
              open >= 1.0f ? 0x60f080 : 0xf07060, 1.0f);
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

static void load_sprite(art_sprite *sprite, const char *root, const char *name)
{
    char path[4096];
    kilix_asset_image image = {0};
    kilix_asset_limits limits;
    kilix_asset_limits_init(&limits);
    limits.max_dimension = 128;
    int len = snprintf(path, sizeof path, "%s/assets/%s", root, name);
    if (len < 0 || (size_t)len >= sizeof path ||
        kilix_asset_image_load_png(&image, path, &limits) != KILIX_ASSET_OK)
        return;

    uint32_t *pixels = malloc((size_t)image.width * image.height * sizeof *pixels);
    if (pixels) {
        for (uint32_t y = 0; y < image.height; y++) {
            const uint8_t *rgba = image.pixels + y * image.stride;
            for (uint32_t x = 0; x < image.width; x++, rgba += 4) {
                unsigned a = rgba[3];
                unsigned r = (rgba[0] * a + 127) / 255;
                unsigned g = (rgba[1] * a + 127) / 255;
                unsigned b = (rgba[2] * a + 127) / 255;
                pixels[(size_t)y * image.width + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        *sprite = (art_sprite){pixels, (int)image.width, (int)image.height};
    }
    kilix_asset_image_clear(&image);
}

void art_load_room(room_art *art, const char *root)
{
    static unsigned revision;
    load_sprite(&art->stone, root, "wall-stone.png");
    load_sprite(&art->brick, root, "wall-brick.png");
    load_sprite(&art->floor, root, "floor.png");
    load_sprite(&art->door, root, "door-leaf.png");
    art->revision = ++revision;
}

void art_free_room(room_art *art)
{
    free(art->stone.pixels);
    free(art->brick.pixels);
    free(art->floor.pixels);
    free(art->door.pixels);
    *art = (room_art){0};
}

void art_load_chest(chest_art *art, const char *root)
{
    load_sprite(&art->closed, root, "chest-closed.png");
    load_sprite(&art->open, root, "chest-open.png");
    load_sprite(&art->key, root, "chest-key.png");
}

void art_free_chest(chest_art *art)
{
    free(art->closed.pixels);
    free(art->open.pixels);
    free(art->key.pixels);
    *art = (chest_art){0};
}

static void draw_chest_placeholder(sr_canvas *c, int x, int y, bool open)
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


void art_chest(sr_canvas *c, const chest_art *art, int x, int y, bool open)
{
    if (!art || !art->closed.pixels || !art->open.pixels || !art->key.pixels) {
        draw_chest_placeholder(c, x, y - 32, open);
        return;
    }
    const art_sprite *sprite = open ? &art->open : &art->closed;
    draw_picture(c, sprite->pixels, sprite->w, sprite->h,
                 x, y - sprite->h, sprite->w, sprite->h, 1.0f);
    if (open) {
        const art_sprite *key = &art->key;
        draw_picture(c, key->pixels, key->w, key->h,
                     x + (sprite->w - key->w) / 2, y - 34, key->w, key->h, 1.0f);
    }
}
