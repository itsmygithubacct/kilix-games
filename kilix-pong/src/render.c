/* Software RGBA renderer for the luminous Kilix Pong arena. */
#include "kilix_pong.h"
#include "font8x16.h"
#include "soft_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *framebuffer;
static sr_canvas canvas;
static int output_width;
static int output_height;
static float logical_scale;
static float logical_origin_x;
static float logical_origin_y;
static float camera_x;
static float camera_y;

static void rectangle_pixels(int x, int y, int width, int height,
                             uint32_t color, float alpha)
{
    sr_fill_rect(&canvas, (float)x, (float)y, (float)width, (float)height,
                 color, alpha);
}

static void circle_pixels(float center_x, float center_y, float radius,
                          uint32_t color, float alpha)
{
    sr_fill_circle(&canvas, center_x, center_y, radius, color, alpha);
}

static void line_pixels(float x0, float y0, float x1, float y1, float width,
                        uint32_t color, float alpha)
{
    sr_line(&canvas, x0, y0, x1, y1, width, color, alpha, 0, 0);
}

static float screen_x(float logical_x)
{
    return logical_origin_x + (logical_x + camera_x) * logical_scale;
}

static float screen_y(float logical_y)
{
    return logical_origin_y + (logical_y + camera_y) * logical_scale;
}

static void rectangle_logical(float x, float y, float width, float height,
                              uint32_t color, float alpha)
{
    int px = (int)floorf(screen_x(x));
    int py = (int)floorf(screen_y(y));
    int pw = (int)ceilf(width * logical_scale);
    int ph = (int)ceilf(height * logical_scale);
    rectangle_pixels(px, py, pw, ph, color, alpha);
}

static void circle_logical(float x, float y, float radius,
                           uint32_t color, float alpha)
{
    circle_pixels(screen_x(x), screen_y(y), radius * logical_scale,
                  color, alpha);
}

static void line_logical(float x0, float y0, float x1, float y1, float width,
                         uint32_t color, float alpha)
{
    line_pixels(screen_x(x0), screen_y(y0), screen_x(x1), screen_y(y1),
                width * logical_scale, color, alpha);
}

static void rounded_rectangle(float x, float y, float width, float height,
                              float radius, uint32_t color, float alpha)
{
    if (radius < 0.0f) radius = 0.0f;
    if (radius * 2.0f > width) radius = width * 0.5f;
    if (radius * 2.0f > height) radius = height * 0.5f;
    rectangle_logical(x + radius, y, width - radius * 2.0f, height,
                      color, alpha);
    rectangle_logical(x, y + radius, width, height - radius * 2.0f,
                      color, alpha);
    circle_logical(x + radius, y + radius, radius, color, alpha);
    circle_logical(x + width - radius, y + radius, radius, color, alpha);
    circle_logical(x + radius, y + height - radius, radius, color, alpha);
    circle_logical(x + width - radius, y + height - radius, radius,
                   color, alpha);
}

static float text_width_logical(const char *text, float pixel_size)
{
    size_t length = text ? strlen(text) : 0;
    return length ? (float)(length * 6 - 1) * pixel_size : 0.0f;
}

static void text_logical(float x, float y, const char *text, float pixel_size,
                         uint32_t color, float alpha)
{
    if (!text || pixel_size <= 0.0f) return;
    for (; *text; text++, x += 6.0f * pixel_size) {
        const uint8_t *rows = kilix_glyph_rows(*text);
        for (int glyph_y = 0; glyph_y < 7; glyph_y++) {
            for (int glyph_x = 0; glyph_x < 5; glyph_x++) {
                if (rows[glyph_y] & (1U << (4 - glyph_x)))
                    rectangle_logical(x + glyph_x * pixel_size,
                                      y + glyph_y * pixel_size,
                                      pixel_size, pixel_size, color, alpha);
            }
        }
    }
}

static void text_centered(float center_x, float y, const char *text,
                          float pixel_size, uint32_t color, float alpha)
{
    text_logical(center_x - text_width_logical(text, pixel_size) * 0.5f,
                 y, text, pixel_size, color, alpha);
}

static void text_right(float right_x, float y, const char *text,
                       float pixel_size, uint32_t color, float alpha)
{
    text_logical(right_x - text_width_logical(text, pixel_size), y, text,
                 pixel_size, color, alpha);
}

static void glowing_text_centered(float center_x, float y, const char *text,
                                  float pixel_size, uint32_t color)
{
    text_centered(center_x + 1.2f, y + 1.2f, text, pixel_size, 0x000000, .72f);
    text_centered(center_x, y, text, pixel_size, color, 1.0f);
}

static void clear_background(void)
{
    if (!framebuffer) return;
    float center_x = output_width * 0.5f;
    float center_y = output_height * 0.48f;
    float maximum_distance = sqrtf(center_x * center_x + center_y * center_y);
    for (int y = 0; y < output_height; y++) {
        float vertical = output_height > 1 ? (float)y / (output_height - 1) : 0;
        for (int x = 0; x < output_width; x++) {
            float dx = x - center_x;
            float dy = y - center_y;
            float vignette = sqrtf(dx * dx + dy * dy) / maximum_distance;
            float glow = clampf(1.0f - vignette, 0.0f, 1.0f);
            int scanline = (y & 3) == 0 ? 2 : 0;
            uint8_t red = (uint8_t)clampf(3.0f + glow * 6.0f + scanline,
                                          0.0f, 255.0f);
            uint8_t green = (uint8_t)clampf(
                7.0f + glow * 11.0f + vertical * 3.0f, 0.0f, 255.0f);
            uint8_t blue = (uint8_t)clampf(
                19.0f + glow * 20.0f + vertical * 5.0f, 0.0f, 255.0f);
            sr_px(&canvas, x, y, sr_rgb(red, green, blue));
        }
    }
}

static void draw_stars(float time)
{
    for (int index = 0; index < 64; index++) {
        int seed_x = (index * 73 + 29) % 311;
        int seed_y = (index * 47 + 13) % 167;
        float pulse = .25f + .20f * sinf(time * (0.7f + (index % 5) * .13f) +
                                         index * 1.37f);
        float size = index % 11 == 0 ? .52f : .28f;
        circle_logical(4.0f + seed_x, 5.0f + seed_y, size, 0x93c5fd,
                       clampf(pulse, .08f, .48f));
    }
}

static void draw_arena(float time)
{
    (void)time;
    rounded_rectangle(3.0f, 3.0f, 314.0f, 174.0f, 3.0f, 0x07132d, .46f);
    line_logical(4, 3, 316, 3, .65f, 0x2dd4bf, .48f);
    line_logical(4, 177, 316, 177, .65f, 0xf472b6, .42f);
    line_logical(3, 5, 3, 175, .55f, 0x22d3ee, .25f);
    line_logical(317, 5, 317, 175, .55f, 0xfb7185, .25f);

    for (int x = 24; x < 320; x += 24)
        line_logical((float)x, 4, (float)x, 176, .25f, 0x3b82f6, .055f);
    for (int y = 24; y < 180; y += 24)
        line_logical(4, (float)y, 316, (float)y, .25f, 0x3b82f6, .055f);
    for (int y = 7; y < 176; y += 10) {
        rounded_rectangle(159.35f, (float)y, 1.3f, 5.5f, .55f,
                          0xc4b5fd, .50f);
    }
    circle_logical(160, 90, 30, 0x8b5cf6, .035f);
    circle_logical(160, 90, 15, 0x22d3ee, .028f);
}

static void draw_paddle_shape(float x, float y, float width, float height,
                              uint32_t color)
{
    rounded_rectangle(x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f,
                      3.5f, color, .07f);
    rounded_rectangle(x - 1.0f, y - 1.0f, width + 2.0f, height + 2.0f,
                      2.5f, color, .16f);
    rounded_rectangle(x, y, width, height, 1.8f, color, .92f);
    rounded_rectangle(x + .75f, y + 1.1f, fmaxf(.7f, width * .24f),
                      height - 2.2f, .7f, 0xffffff, .48f);
}

static void draw_ball_shape(float x, float y, float radius)
{
    circle_logical(x, y, radius * 3.2f, 0xfbbf24, .045f);
    circle_logical(x, y, radius * 2.0f, 0xfde68a, .15f);
    circle_logical(x, y, radius, 0xfff7d6, 1.0f);
    circle_logical(x - radius * .28f, y - radius * .30f,
                   radius * .30f, 0xffffff, .95f);
}

static void draw_particles(void)
{
    for (int index = 0; index < MAX_PARTICLES; index++) {
        const Particle *particle = &G.particles[index];
        if (!particle->active || particle->max_life <= 0.0f) continue;
        float life = clampf(particle->life / particle->max_life, 0.0f, 1.0f);
        /* game.c emits particle colors as 0xRRGGBBAA. Accept a 24-bit RGB
           value too so hand-authored render fixtures remain convenient. */
        uint32_t color = particle->color;
        float encoded_alpha = 1.0f;
        if (color > 0x00ffffffU) {
            encoded_alpha = (color & 255U) / 255.0f;
            color >>= 8;
        }
        circle_logical(particle->x, particle->y, .45f + life * .85f,
                       color, life * .86f * encoded_alpha);
        if (life > .55f)
            circle_logical(particle->x, particle->y, 2.2f,
                           color, (life - .55f) * .10f * encoded_alpha);
    }
}

static void draw_ball_and_trail(void)
{
    if (!G.ball.active) return;
    for (int age = BALL_TRAIL_LEN - 1; age >= 0; age--) {
        int index = (G.ball.trail_head - age + BALL_TRAIL_LEN) % BALL_TRAIL_LEN;
        float x = G.ball.trail_x[index];
        float y = G.ball.trail_y[index];
        if (x <= 0.0f || x >= LOGICAL_W || y <= 0.0f || y >= LOGICAL_H)
            continue;
        float strength = (float)(BALL_TRAIL_LEN - age) / BALL_TRAIL_LEN;
        circle_logical(x, y, BALL_RADIUS * (.28f + strength * .44f),
                       0xfde68a, strength * .26f);
    }
    draw_ball_shape(G.ball.x, G.ball.y, BALL_RADIUS);
}

/* Scoreboard label for a side, from its controller. */
static const char *side_name(int side)
{
    static char labels[SIDE_COUNT][24];
    int controller = G.paddles[side].controller;
    bool two_humans = G.paddles[SIDE_LEFT].controller == CTRL_HUMAN &&
                      G.paddles[SIDE_RIGHT].controller == CTRL_HUMAN;
    if (controller == CTRL_HUMAN)
        return two_humans ? (side == SIDE_LEFT ? "PLAYER 1" : "PLAYER 2")
                          : "PLAYER";
    if (controller == CTRL_NEURAL && game_neural_ready()) return "NEURAL";
    /* NEURAL without a loaded policy plays as the CPU; say so. */
    (void)snprintf(labels[side], sizeof labels[side], "CPU %s",
                   game_level_name(G.level));
    return labels[side];
}

static void draw_scores(void)
{
    char left[16];
    char right[16];
    (void)snprintf(left, sizeof left, "%02d", G.paddles[SIDE_LEFT].score);
    (void)snprintf(right, sizeof right, "%02d", G.paddles[SIDE_RIGHT].score);
    text_right(148, 10, left, 2.8f, 0x67e8f9, .26f);
    text_logical(172, 10, right, 2.8f, 0xf9a8d4, .26f);
    text_right(146, 34, side_name(SIDE_LEFT), .52f, 0x67e8f9, .76f);
    text_logical(174, 34, side_name(SIDE_RIGHT), .52f, 0xf9a8d4, .76f);

    if (G.ball.active && G.state == GS_PLAYING) {
        /* Ball speed relative to this match's serve, so the per-hit speed-up
           is visible; it turns from cyan through amber to red near the cap. */
        char speed[24];
        float serve = game_serve_speed();
        (void)snprintf(speed, sizeof speed, "SPEED %.1fX",
                       (double)(G.ball.speed / (serve > 0.0f ? serve : 1.0f)));
        float heat = clampf((G.ball.speed - serve) /
                            (BALL_SPEED_MAX - serve > 1.0f ? BALL_SPEED_MAX - serve : 1.0f),
                            0.0f, 1.0f);
        uint32_t color = heat < .35f ? 0x67e8f9 : (heat < .75f ? 0xfbbf24 : 0xf87171);
        text_right(311, 166.4f, speed, .45f, color, .85f);
    }

    if (G.rally >= 3) {
        char rally[32];
        (void)snprintf(rally, sizeof rally, "RALLY %d", G.rally);
        rounded_rectangle(135, 158, 50, 12, 3, 0x0f172a, .74f);
        text_centered(160, 161.2f, rally, .58f, 0xfde68a, .90f);
    }
}

static void draw_game_objects(void)
{
    draw_paddle_shape(G.paddles[SIDE_LEFT].x, G.paddles[SIDE_LEFT].y,
                      G.paddles[SIDE_LEFT].w, G.paddles[SIDE_LEFT].h,
                      0x22d3ee);
    draw_paddle_shape(G.paddles[SIDE_RIGHT].x, G.paddles[SIDE_RIGHT].y,
                      G.paddles[SIDE_RIGHT].w, G.paddles[SIDE_RIGHT].h,
                      0xf472b6);
    draw_ball_and_trail();
    draw_particles();
}

static void draw_panel(float x, float y, float width, float height)
{
    rounded_rectangle(x + 1.5f, y + 1.8f, width, height, 5.0f,
                      0x000000, .55f);
    rounded_rectangle(x, y, width, height, 5.0f, 0x071226, .94f);
    line_logical(x + 5, y, x + width - 5, y, .7f, 0x67e8f9, .66f);
    line_logical(x + 5, y + height, x + width - 5, y + height,
                 .7f, 0xf472b6, .55f);
}

/* A centered list of actions; the selected one sits on a lit bar. */
static void draw_menu_items(const char *const *items, int count, int selected,
                            float top, float pitch, float time)
{
    float pulse = .70f + .30f * sinf(time * 5.0f);
    for (int index = 0; index < count; index++) {
        float y = top + (float)index * pitch;
        if (index == selected) {
            rounded_rectangle(112, y - 2.6f, 96, 9.6f, 3, 0x1e293b, .95f);
            line_logical(114, y + 7.0f, 206, y + 7.0f, .45f, 0xfde68a, .55f * pulse);
            text_logical(116, y, ">", .52f, 0xfde68a, pulse);
        }
        text_centered(160, y, items[index], .52f,
                      index == selected ? 0xfef3c7 : 0xcbd5e1,
                      index == selected ? 1.0f : .70f);
    }
}

static void draw_status_overlays(float time)
{
    camera_x = camera_y = 0.0f;
    if (G.state == GS_SERVE) {
        draw_panel(93, 63, 134, 54);
        text_centered(160, 72, side_name(G.serve_to), .68f,
                      G.serve_to == SIDE_LEFT ? 0x67e8f9 : 0xf9a8d4, 1);
        glowing_text_centered(160, 84, "RECEIVES", 1.12f, 0xfef3c7);
        float pulse = .62f + .38f * sinf(time * 4.2f);
        text_centered(160, 105, "GET READY", .54f, 0xcbd5e1, pulse);
    } else if (G.state == GS_POINT) {
        float pulse = .78f + .22f * sinf(time * 8.0f);
        rounded_rectangle(119, 71, 82, 35, 5, 0x071226, .80f);
        text_centered(160, 81, "POINT", 1.55f, 0xfef3c7, pulse);
    } else if (G.state == GS_PAUSED) {
        static const char *items[PAUSE_ROWS] = {
            "RESUME", "RESTART MATCH", "MAIN MENU", "QUIT GAME"
        };
        rectangle_logical(3, 3, 314, 174, 0x020617, .50f);
        draw_panel(100, 46, 120, 90);
        glowing_text_centered(160, 55, "PAUSED", 1.35f, 0xfde68a);
        draw_menu_items(items, PAUSE_ROWS, G.pause_row, 78, 11, time);
        text_centered(160, 126, "P OR ESC RESUMES", .40f, 0x94a3b8, .85f);
    }
}

static void draw_title(float time)
{
    draw_arena(time);
    float left_y = 90.0f + sinf(time * 1.15f) * 23.0f;
    float right_y = 90.0f + cosf(time * 1.03f) * 20.0f;
    draw_paddle_shape(25, left_y - 15, PADDLE_W, PADDLE_H, 0x22d3ee);
    draw_paddle_shape(291, right_y - 15, PADDLE_W, PADDLE_H, 0xf472b6);
    float ball_x = 160.0f + sinf(time * .78f) * 92.0f;
    float ball_y = 102.0f + sinf(time * 1.61f) * 27.0f;
    draw_ball_shape(ball_x, ball_y, BALL_RADIUS);

    rounded_rectangle(66, 6, 188, 168, 8, 0x020617, .84f);
    text_centered(120.6f, 12.6f, "KILIX", 1.9f, 0x082f49, .90f);
    text_centered(120, 12, "KILIX", 1.9f, 0x67e8f9, 1.0f);
    text_centered(196.6f, 12.6f, "PONG", 1.9f, 0x4c0519, .90f);
    text_centered(196, 12, "PONG", 1.9f, 0xf9a8d4, 1.0f);
    line_logical(80, 30, 240, 30, .45f, 0x8b5cf6, .45f);

    /* Action rows (PLAY, QUIT) are centered; value rows show label and value,
       with arrows on the selected one. */
    static const char *labels[MENU_ROWS] = {
        "PLAY", "LEFT PADDLE", "RIGHT PADDLE", "CPU LEVEL", "SPEED-UP",
        "SERVE SPEED", "POINTS TO WIN", "PADDLE SIZE", "SOUND", "QUIT GAME"
    };
    bool cpu_in_match = G.setup[SIDE_LEFT] == CTRL_CPU ||
                        G.setup[SIDE_RIGHT] == CTRL_CPU ||
                        (!game_neural_ready() &&
                         (G.setup[SIDE_LEFT] == CTRL_NEURAL ||
                          G.setup[SIDE_RIGHT] == CTRL_NEURAL));
    float pulse = .70f + .30f * sinf(time * 5.0f);
    for (int row = 0; row < MENU_ROWS; row++) {
        float y = 36.0f + (float)row * 12.0f + (row > MENU_PLAY ? 3.0f : 0.0f) +
                  (row == MENU_QUIT ? 3.0f : 0.0f);
        bool selected = row == G.menu_row;
        if (selected) {
            rounded_rectangle(76, y - 2.8f, 168, 10.0f, 3, 0x1e293b, .95f);
            line_logical(78, y + 7.2f, 242, y + 7.2f, .45f, 0xfde68a, .55f * pulse);
        }
        if (row == MENU_PLAY || row == MENU_QUIT) {
            uint32_t color = row == MENU_PLAY ? 0xfef3c7 : 0xfda4af;
            text_centered(160, y, labels[row], row == MENU_PLAY ? .66f : .52f, color,
                          selected ? pulse : .70f);
            continue;
        }
        char value[24];
        const char *text;
        switch (row) {
        case MENU_LEFT:  text = game_controller_name(G.setup[SIDE_LEFT]); break;
        case MENU_RIGHT: text = game_controller_name(G.setup[SIDE_RIGHT]); break;
        case MENU_LEVEL: text = game_level_name(G.level); break;
        case MENU_SOUND: text = G.sound_on ? "ON" : "OFF"; break;
        default:
            text = game_option_name(OPT_SPEEDUP + (row - MENU_SPEEDUP),
                                    G.option[OPT_SPEEDUP + (row - MENU_SPEEDUP)]);
            break;
        }
        (void)snprintf(value, sizeof value, selected ? "< %s >" : "%s", text);
        uint32_t color = row == MENU_LEFT ? 0x67e8f9
                         : (row == MENU_RIGHT ? 0xf9a8d4 : 0xe2e8f0);
        float alpha = (row == MENU_LEVEL && !cpu_in_match) ? .45f : 1.0f;
        alpha *= selected ? 1.0f : .72f;
        text_logical(84, y, labels[row], .50f, 0x94a3b8, alpha);
        text_right(selected ? 236 : 229.6f, y, value, .50f,
                   selected ? 0xfde68a : color, alpha);
    }
    text_centered(160, 166, "UP DOWN SELECT   LEFT RIGHT CHANGE   ENTER OK", .36f,
                  0x94a3b8, .85f);
}

static void draw_gameover(float time)
{
    draw_arena(time);
    draw_game_objects();
    rectangle_logical(3, 3, 314, 174, 0x020617, .56f);
    draw_panel(80, 32, 160, 124);
    text_centered(160, 43, "MATCH", .72f, 0x94a3b8, .95f);
    glowing_text_centered(160, 57, side_name(G.winner), 1.20f,
                          G.winner == SIDE_LEFT ? 0x67e8f9 : 0xf9a8d4);
    text_centered(160, 73, "WINS", 1.55f, 0xfef3c7, 1.0f);
    char score[32];
    (void)snprintf(score, sizeof score, "%02d  -  %02d",
                   G.paddles[SIDE_LEFT].score,
                   G.paddles[SIDE_RIGHT].score);
    text_centered(160, 96, score, .82f, 0xe2e8f0, .95f);
    static const char *items[OVER_ROWS] = { "REMATCH", "MAIN MENU", "QUIT GAME" };
    draw_menu_items(items, OVER_ROWS, G.over_row, 118, 11, time);
}

void render_init(int width, int height)
{
    free(framebuffer);
    framebuffer = NULL;
    sr_canvas_free(&canvas);
    output_width = output_height = 0;
    render_resize(width, height);
}

void render_resize(int width, int height)
{
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / (size_t)height / 4) {
        free(framebuffer);
        framebuffer = NULL;
        output_width = output_height = 0;
        logical_scale = 0.0f;
        return;
    }
    size_t required = (size_t)width * (size_t)height * 4;
    uint8_t *resized = realloc(framebuffer, required);
    if (!resized) {
        free(framebuffer);
        framebuffer = NULL;
        output_width = output_height = 0;
        logical_scale = 0.0f;
        return;
    }
    framebuffer = resized;
    sr_canvas_free(&canvas);
    if (!sr_canvas_init(&canvas, width, height)) {
        free(framebuffer);
        framebuffer = NULL;
        output_width = output_height = 0;
        logical_scale = 0.0f;
        return;
    }
    output_width = width;
    output_height = height;
    logical_scale = fminf(width / LOGICAL_W, height / LOGICAL_H);
    logical_origin_x = (width - LOGICAL_W * logical_scale) * .5f;
    logical_origin_y = (height - LOGICAL_H * logical_scale) * .5f;
}

void render_shutdown(void)
{
    free(framebuffer);
    framebuffer = NULL;
    sr_canvas_free(&canvas);
    output_width = output_height = 0;
    logical_scale = 0.0f;
}

uint8_t *render_fb(void)
{
    if (framebuffer != NULL)
        (void)sr_pack_rgba(&canvas, framebuffer,
                           (size_t)output_width * (size_t)output_height * 4u);
    return framebuffer;
}

void render_frame(void)
{
    if (!framebuffer || logical_scale <= 0.0f) return;
    clear_background();
    camera_x = camera_y = 0.0f;
    float time = (float)G.ticks * TICK_DT;
    draw_stars(time);

    if (G.state == GS_TITLE) {
        draw_title(time);
    } else if (G.state == GS_GAMEOVER) {
        draw_gameover(time);
    } else {
        if (G.shake > 0.0f) {
            float amount = clampf(G.shake, 0.0f, 1.0f) * 1.7f;
            camera_x = sinf((float)G.ticks * 2.17f) * amount;
            camera_y = cosf((float)G.ticks * 1.63f) * amount * .58f;
        }
        draw_arena(time);
        draw_game_objects();
        camera_x = camera_y = 0.0f;
        draw_scores();
        draw_status_overlays(time);
    }

    camera_x = camera_y = 0.0f;
    if (!G.sound_on) {
        rounded_rectangle(7, 164, 39, 9, 2.5f, 0x0f172a, .80f);
        text_logical(11, 166.4f, "SOUND OFF", .40f, 0x94a3b8, .90f);
    }
    if (G.flash > 0.0f)
        rectangle_logical(0, 0, LOGICAL_W, LOGICAL_H, 0xfff1c2,
                          clampf(G.flash * .42f, 0.0f, .48f));
}

bool render_dump_ppm(const char *path)
{
    return path != NULL && *path != '\0' && sr_write_ppm(&canvas, path);
}
