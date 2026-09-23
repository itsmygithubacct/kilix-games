/* app.c - tactile switch board, sidebar controls, help, and victory UI. */
#include "app.h"

#include "art.h"
#include "draw.h"
#include "font.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    TARGET_NONE = -1,
    TARGET_MODE_3 = 100,
    TARGET_MODE_5,
    TARGET_MODE_7,
    TARGET_SOUND,
    TARGET_HELP,
    TARGET_NEW,
    TARGET_RESET,
    TARGET_UNDO
};

typedef struct Control {
    int target;
    GameRect rect;
    const char *label;
} Control;

static const Control controls[] = {
    {TARGET_MODE_3, {438, 118, 100, 20}, "3X3 STARTER"},
    {TARGET_MODE_5, {438, 142, 100, 20}, "5X5 CLASSIC"},
    {TARGET_MODE_7, {438, 166, 100, 20}, "7X7 EXPERT"},
    {TARGET_SOUND,  {438, 193, 100, 20}, "SOUND"},
    {TARGET_HELP,   {438, 217, 100, 18}, "? HELP"},
    {TARGET_NEW,    {438, 260, 100, 20}, "NEW PUZZLE"},
    {TARGET_RESET,  {438, 285, 100, 20}, "RESET"},
    {TARGET_UNDO,   {438, 310, 100, 20}, "UNDO"}
};

enum { CONTROL_COUNT = (int)(sizeof controls / sizeof controls[0]) };

static bool point_in(GameRect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

static int mode_target(int size)
{
    return size == 3 ? TARGET_MODE_3 :
           (size == 5 ? TARGET_MODE_5 : TARGET_MODE_7);
}

void app_init(App *app)
{
    memset(app, 0, sizeof *app);
    game_init(&app->game);
    app->hover_target = TARGET_NONE;
    app->captured_target = TARGET_NONE;
}

int app_target_at(const App *app, int x, int y)
{
    if (app->game.help) {
        GameRect close = {184, 292, 112, 24};
        return point_in(close, x, y) ? TARGET_HELP : TARGET_NONE;
    }

    for (int i = 0; i < CONTROL_COUNT; i++)
        if (point_in(controls[i].rect, x, y)) return controls[i].target;

    if (app->game.won) {
        GameRect next = {172, 250, 136, 25};
        return point_in(next, x, y) ? TARGET_NEW : TARGET_NONE;
    }

    for (int cell = 0; cell < app->game.size * app->game.size; cell++) {
        GameRect rect = game_cell_rect(&app->game, cell);
        if (art_switch_hit(rect.x, rect.y, rect.w, x, y)) return cell;
    }
    return TARGET_NONE;
}

static void activate_cell(App *app, int cell)
{
    bool ended_on = false;
    if (game_activate(&app->game, cell, &ended_on))
        sound_play_switch(ended_on);
}

static void activate_target(App *app, int target)
{
    if (target >= 0 && target < MAX_CELLS) {
        activate_cell(app, target);
        return;
    }
    switch (target) {
    case TARGET_MODE_3: game_set_size(&app->game, 3); break;
    case TARGET_MODE_5: game_set_size(&app->game, 5); break;
    case TARGET_MODE_7: game_set_size(&app->game, 7); break;
    case TARGET_SOUND:
        sound_set_enabled(!sound_is_enabled());
        break;
    case TARGET_HELP:
        app->game.help = !app->game.help;
        break;
    case TARGET_NEW: game_new(&app->game); break;
    case TARGET_RESET: game_reset(&app->game); break;
    case TARGET_UNDO: (void)game_undo(&app->game); break;
    default: break;
    }
}

static void move_focus(Game *game, int dx, int dy)
{
    int row = game->focus_cell / game->size;
    int col = game->focus_cell % game->size;
    row = (row + dy + game->size) % game->size;
    col = (col + dx + game->size) % game->size;
    game->focus_cell = row * game->size + col;
    game->keyboard_focus = true;
}

static AppAction handle_key(App *app, uint32_t key)
{
    Game *game = &app->game;
    app->captured_target = TARGET_NONE;
    game->pressed_cell = -1;
    if (key == KEY_ESCAPE) {
        if (game->help) {
            game->help = false;
            return APP_CONTINUE;
        }
        return APP_QUIT;
    }
    if (key == 'q' || key == 'Q') return APP_QUIT;
    if (key == '?' || key == 'h' || key == 'H') {
        game->help = !game->help;
        return APP_CONTINUE;
    }
    if (game->help) return APP_CONTINUE;

    if (key == KEY_LEFT || key == 'a' || key == 'A') move_focus(game, -1, 0);
    else if (key == KEY_RIGHT || key == 'd' || key == 'D') move_focus(game, 1, 0);
    else if (key == KEY_UP || key == 'w' || key == 'W') move_focus(game, 0, -1);
    else if (key == KEY_DOWN || key == 's' || key == 'S') move_focus(game, 0, 1);
    else if (key == KEY_ENTER || key == ' ') {
        if (game->won) game_new(game);
        else activate_cell(app, game->focus_cell);
        game->keyboard_focus = true;
    } else if (key == 'n' || key == 'N') game_new(game);
    else if (key == 'r' || key == 'R') game_reset(game);
    else if (key == 'u' || key == 'U' || key == KEY_BACKSPACE)
        (void)game_undo(game);
    else if (key == 'm' || key == 'M')
        sound_set_enabled(!sound_is_enabled());
    else if (key == '3') {
        game_set_size(game, 3);
        game->keyboard_focus = true;
    } else if (key == '5') {
        game_set_size(game, 5);
        game->keyboard_focus = true;
    } else if (key == '7') {
        game_set_size(game, 7);
        game->keyboard_focus = true;
    }
    return APP_CONTINUE;
}

AppAction app_handle(App *app, const input_event *event)
{
    int target;
    if (event->kind == IN_KEY_DOWN || event->kind == IN_KEY_REPEAT)
        return handle_key(app, event->key);
    if (event->kind == IN_MOUSE_LEAVE) {
        app->mouse_in_view = false;
        app->hover_target = TARGET_NONE;
        app->captured_target = TARGET_NONE;
        app->game.hover_cell = -1;
        app->game.pressed_cell = -1;
        return APP_CONTINUE;
    }
    if (event->kind != IN_MOUSE_MOVE && event->kind != IN_MOUSE_DOWN &&
        event->kind != IN_MOUSE_UP)
        return APP_CONTINUE;

    app->mouse_x = event->mx;
    app->mouse_y = event->my;
    app->mouse_in_view = event->in_view;
    target = event->in_view ? app_target_at(app, event->mx, event->my) :
                              TARGET_NONE;
    app->hover_target = target;
    app->game.hover_cell = target >= 0 && target < MAX_CELLS ? target : -1;
    if (event->kind == IN_MOUSE_MOVE) {
        app->game.keyboard_focus = false;
    } else if (event->kind == IN_MOUSE_DOWN && event->button == 0) {
        app->captured_target = target;
        app->game.pressed_cell = target >= 0 && target < MAX_CELLS ? target : -1;
        app->game.keyboard_focus = false;
    } else if (event->kind == IN_MOUSE_UP && event->button == 0) {
        int captured = app->captured_target;
        app->captured_target = TARGET_NONE;
        app->game.pressed_cell = -1;
        if (captured != TARGET_NONE && target == captured)
            activate_target(app, captured);
    }
    return APP_CONTINUE;
}

void app_update(App *app, double dt)
{
    game_update(&app->game, dt);
}

static void text_shadow(Canvas *canvas, int x, int y, const char *text,
                        uint32_t color)
{
    draw_text(canvas, x + 1, y + 1, text, 0x081012);
    draw_text(canvas, x, y, text, color);
}

static void text_center_shadow(Canvas *canvas, int cx, int y,
                               const char *text, uint32_t color)
{
    int x = cx - draw_text_width(text) / 2;
    text_shadow(canvas, x, y, text, color);
}

static void draw_control(Canvas *canvas, const App *app,
                         const Control *control)
{
    GameRect r = control->rect;
    bool hovered = app->hover_target == control->target;
    bool pressed = app->captured_target == control->target;
    bool active = control->target == mode_target(app->game.size);
    uint32_t edge = active ? 0xe7bc63 : (hovered ? 0xf5d789 : 0x8f7138);
    uint32_t fill = active ? 0x79501f : (pressed ? 0x0c1c20 : 0x17353a);
    const char *label = control->label;
    char sound_label[16];

    if (control->target == TARGET_SOUND) {
        snprintf(sound_label, sizeof sound_label, "SOUND %s",
                 sound_is_enabled() ? "ON" : "OFF");
        label = sound_label;
    }
    if (control->target == TARGET_UNDO && app->game.history_len == 0)
        fill = 0x17262a;

    draw_round_rect(canvas, r.x, r.y, r.w, r.h, 5, edge);
    draw_round_rect(canvas, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 4, fill);
    text_center_shadow(canvas, r.x + r.w / 2, r.y + (r.h - FONT_CAP_H) / 2,
                       label, active ? 0xfff0bd : 0xe9e0ce);
}

static void draw_lever(Canvas *canvas, GameRect rect, bool on, bool pressed)
{
    int width = imaxi(3, rect.w / 9);
    int height = imaxi(10, rect.h / 4);
    int cx = rect.x + rect.w / 2;
    int cy = rect.y + rect.h / 2 + (pressed ? 1 : 0);
    int top = on ? cy - height + 2 : cy - 2;
    uint32_t body = pressed ? 0x110e0b : 0x261d17;
    draw_round_rect(canvas, cx - width / 2, top, width, height,
                    imaxi(1, width / 2), body);
    draw_rect(canvas, cx - width / 2 + 1, top + 2, 1,
              imaxi(2, height - 5), on ? 0xdab46d : 0x74644f);
    draw_disc(canvas, cx, on ? top + 2 : top + height - 3,
              imaxi(1, width / 3), on ? 0xf5d489 : 0x3b3229);
}

static void draw_board(Canvas *canvas, const App *app)
{
    const Game *game = &app->game;
    char line[64];
    int lit = game_lit_count(game);
    double pulse = 0.5 + 0.5 * sin(game->pulse * 3.2);

    snprintf(line, sizeof line, "%d CIRCUITS LIVE - CUT THEM ALL", lit);
    draw_blend_rect(canvas, 107, 51, 266, 22, 0x081719, 170u);
    draw_round_rect(canvas, 108, 52, 264, 20, 6, 0x19383b);
    draw_frame(canvas, 108, 52, 264, 20, 1, 0x8d7039);
    text_center_shadow(canvas, 240, 55, line,
                       lit == 0 ? 0x9ff0c3 : 0xf0d79c);

    for (int cell = 0; cell < game->size * game->size; cell++) {
        GameRect r = game_cell_rect(game, cell);
        bool on = game_cell_on(game, cell);
        bool hover = app->hover_target == cell;
        bool focus = game->keyboard_focus && game->focus_cell == cell;
        bool pressed = game->pressed_cell == cell;
        int bounce = game->cell_anim[cell] > 0.5 ? 1 : 0;
        r.y += bounce;
        if (on) {
            int glow = (int)(26.0 + pulse * 12.0);
            draw_blend_disc(canvas, r.x + r.w / 2, r.y + r.h / 2,
                            r.w * 2 / 3, 0xffb43d, (unsigned)glow);
            draw_blend_disc(canvas, r.x + r.w / 2, r.y + r.h / 2,
                            r.w / 2, 0xffdc77, 28u);
        }
        art_draw_switch(canvas, r.x, r.y, r.w, on, hover, focus, pressed);
        draw_lever(canvas, r, on, pressed);
    }
}

static const char *tooltip_for(const App *app, char *buffer, size_t size)
{
    int target = app->hover_target;
    if (app->game.help) return "ESC OR CLICK CLOSE TO RETURN TO THE BOARD";
    if (app->game.won) return "EVERY CIRCUIT IS DARK - THE WORKSHOP IS QUIET";
    if (target >= 0 && target < MAX_CELLS) {
        snprintf(buffer, size, "SWITCH R%d C%d - FLIPS ITS NEIGHBORS",
                 target / app->game.size + 1, target % app->game.size + 1);
        return buffer;
    }
    switch (target) {
    case TARGET_MODE_3: return "SMALL BOARD FOR LEARNING THE CIRCUIT";
    case TARGET_MODE_5: return "THE CLASSIC 5 BY 5 CIRCUIT";
    case TARGET_MODE_7: return "A DENSE 49-SWITCH CHALLENGE";
    case TARGET_SOUND: return "TOGGLE THE MECHANICAL SWITCH SOUND";
    case TARGET_HELP: return "SHOW CONTROLS AND PUZZLE RULES";
    case TARGET_NEW: return "WIRE A NEW SOLVABLE PUZZLE";
    case TARGET_RESET: return "RESTORE THIS PUZZLE TO ITS START";
    case TARGET_UNDO: return "TAKE BACK THE LAST SWITCH";
    default: return "CLICK A SWITCH - IT FLIPS ORTHOGONAL NEIGHBORS";
    }
}

static void draw_sidebar(Canvas *canvas, const App *app)
{
    char line[32];
    int mode = game_mode_index(app->game.size);
    text_center_shadow(canvas, 488, 70, "LIGHTS", 0xffdda0);
    text_center_shadow(canvas, 488, 87, "WORKSHOP", 0xa8c7c5);
    snprintf(line, sizeof line, "PUZZLE %03d", app->game.puzzle_number);
    text_center_shadow(canvas, 488, 101, line, 0xd7c89e);

    for (int i = 0; i < CONTROL_COUNT; i++) draw_control(canvas, app, &controls[i]);

    snprintf(line, sizeof line, "MOVES %03d", app->game.moves);
    text_center_shadow(canvas, 488, 241, line, 0xf0d79c);
    if (app->game.best[mode] >= 0) {
        snprintf(line, sizeof line, "BEST %03d", app->game.best[mode]);
        text_center_shadow(canvas, 488, 342, line, 0x9fd8b7);
    }
}

static void draw_bottom_tip(Canvas *canvas, const App *app)
{
    char buffer[80];
    const char *tip = tooltip_for(app, buffer, sizeof buffer);
    int width = draw_text_width(tip) + 20;
    int x = 320 - width / 2;
    draw_blend_rect(canvas, x, 365, width, 22, 0x070d0d, 190u);
    draw_round_rect(canvas, x + 1, 366, width - 2, 20, 6, 0x142526);
    text_center_shadow(canvas, 320, 369, tip, 0xe6d8bc);
}

static void draw_help(Canvas *canvas, const App *app)
{
    GameRect close = {184, 292, 112, 24};
    bool hover = app->hover_target == TARGET_HELP;
    draw_blend_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, 0x020809, 95u);
    draw_shadow(canvas, 76, 70, 328, 258);
    draw_round_rect(canvas, 76, 70, 328, 258, 12, 0xc99a4b);
    draw_round_rect(canvas, 80, 74, 320, 250, 10, 0x10282c);
    text_center_shadow(canvas, 240, 88, "HOW TO CUT THE LIGHTS", 0xffd77d);
    text_center_shadow(canvas, 240, 116,
                       "EVERY SWITCH FLIPS ITSELF PLUS", 0xe8e0ce);
    text_center_shadow(canvas, 240, 134,
                       "THE LIGHTS ABOVE BELOW LEFT RIGHT", 0xe8e0ce);
    text_center_shadow(canvas, 240, 157,
                       "WIN WHEN EVERY CIRCUIT IS DARK", 0x9ee5bd);
    text_shadow(canvas, 111, 188, "ARROWS / WASD", 0xf0d39b);
    text_shadow(canvas, 270, 188, "MOVE FOCUS", 0xc7d7d5);
    text_shadow(canvas, 111, 208, "ENTER / SPACE", 0xf0d39b);
    text_shadow(canvas, 270, 208, "FLIP SWITCH", 0xc7d7d5);
    text_shadow(canvas, 111, 228, "N  R  U  M", 0xf0d39b);
    text_shadow(canvas, 270, 228, "NEW RESET UNDO MUTE", 0xc7d7d5);
    text_shadow(canvas, 111, 248, "3  5  7", 0xf0d39b);
    text_shadow(canvas, 270, 248, "BOARD SIZE", 0xc7d7d5);
    draw_round_rect(canvas, close.x, close.y, close.w, close.h, 6,
                    hover ? 0xffd77d : 0x9b7539);
    draw_round_rect(canvas, close.x + 2, close.y + 2,
                    close.w - 4, close.h - 4, 5, 0x18393d);
    text_center_shadow(canvas, 240, 297, "CLOSE", 0xf4ead5);
}

static void draw_victory(Canvas *canvas, const App *app)
{
    char line[40];
    GameRect next = {172, 250, 136, 25};
    bool hover = app->hover_target == TARGET_NEW;
    double t = app->game.win_time;

    for (int i = 0; i < 24; i++) {
        double angle = (double)i * 2.399963 + t * (0.35 + (i % 3) * 0.1);
        double radius = 52.0 + fmod(t * (28.0 + i), 98.0);
        int x = 240 + (int)(cos(angle) * radius);
        int y = 205 + (int)(sin(angle) * radius * 0.68);
        draw_disc(canvas, x, y, i % 4 == 0 ? 2 : 1,
                  i % 3 == 0 ? 0xffcc62 : (i % 3 == 1 ? 0x7de3ba : 0x72cbe7));
    }

    draw_blend_rect(canvas, 0, 0, 444, 356, 0x020707, 88u);
    draw_shadow(canvas, 94, 132, 292, 154);
    draw_round_rect(canvas, 94, 132, 292, 154, 12, 0xd6a84f);
    draw_round_rect(canvas, 98, 136, 284, 146, 10, 0x10272a);
    text_center_shadow(canvas, 240, 153, "ALL LIGHTS OUT", 0xffdd7c);
    text_center_shadow(canvas, 240, 177, "THE WORKSHOP IS QUIET", 0x9be6c0);
    snprintf(line, sizeof line, "SOLVED IN %d MOVES", app->game.moves);
    text_center_shadow(canvas, 240, 207, line, 0xf0e4cf);
    draw_round_rect(canvas, next.x, next.y, next.w, next.h, 6,
                    hover ? 0xffdc7d : 0xa67d3d);
    draw_round_rect(canvas, next.x + 2, next.y + 2,
                    next.w - 4, next.h - 4, 5, 0x1b4144);
    text_center_shadow(canvas, 240, 255, "NEW PUZZLE", 0xfff0d5);
}

void app_draw(const App *app, Canvas *canvas)
{
    art_draw_room(canvas);
    draw_board(canvas, app);
    draw_sidebar(canvas, app);
    draw_bottom_tip(canvas, app);
    if (app->game.won) draw_victory(canvas, app);
    if (app->game.help) draw_help(canvas, app);
}

static void send_mouse(App *app, input_kind kind, int x, int y,
                       uint8_t button)
{
    input_event event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.mx = (int16_t)x;
    event.my = (int16_t)y;
    event.button = button;
    event.in_view = true;
    (void)app_handle(app, &event);
}

bool app_interaction_selftest(void)
{
    App app;
    uint64_t before;
    GameRect center;
    GameRect neighbor;
    int x;
    int y;

    app_init(&app);
    sound_reset_trace();
    center = game_cell_rect(&app.game, 12);
    x = center.x + center.w / 2;
    y = center.y + center.h / 2;
    if (app_target_at(&app, x, y) != 12) return false;
    before = app.game.board;
    send_mouse(&app, IN_MOUSE_DOWN, x, y, 0);
    send_mouse(&app, IN_MOUSE_UP, x, y, 0);
    if (app.game.board != (before ^ game_toggle_mask(5, 12)) ||
        app.game.moves != 1 || sound_trace_count() != 1u)
        return false;

    /* Release outside the captured item must not activate. */
    before = app.game.board;
    neighbor = game_cell_rect(&app.game, 13);
    send_mouse(&app, IN_MOUSE_DOWN, x, y, 0);
    send_mouse(&app, IN_MOUSE_UP, neighbor.x + neighbor.w / 2,
               neighbor.y + neighbor.h / 2, 0);
    if (app.game.board != before || sound_trace_count() != 1u) return false;

    /* Right clicks and the transparent corner of the sprite are inert. */
    send_mouse(&app, IN_MOUSE_DOWN, x, y, 2);
    send_mouse(&app, IN_MOUSE_UP, x, y, 2);
    if (app.game.board != before || sound_trace_count() != 1u) return false;
    if (app_target_at(&app, center.x, center.y) == 12) return false;

    activate_target(&app, TARGET_RESET);
    if (app.game.board != app.game.initial || app.game.moves != 0) return false;
    activate_target(&app, TARGET_MODE_3);
    if (app.game.size != 3) return false;
    activate_target(&app, TARGET_MODE_7);
    if (app.game.size != 7) return false;
    activate_target(&app, TARGET_MODE_5);
    if (app.game.size != 5) return false;
    return true;
}
