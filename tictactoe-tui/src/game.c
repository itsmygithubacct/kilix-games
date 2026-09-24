/* The app: menus, turns, the computer's move delay, and the running score. */
#include "ttt.h"

#include <stdio.h>
#include <string.h>

void game_init(Game *g, uint32_t seed)
{
    memset(g, 0, sizeof *g);
    g->screen = SCREEN_MENU;
    g->menu_row = MENU_PLAY;
    g->players[0] = PLAYER_HUMAN;
    g->players[1] = PLAYER_NEURAL;
    g->level = LEVEL_PERFECT;
    g->starts = STARTS_X;
    g->rng = seed ? seed : 0x2545f491u;
    g->cursor = 4;
    g->hint = -1;
    board_init(&g->board, MARK_X);
}

static int mover_index(const Game *g)
{
    return board_to_move(&g->board) == MARK_X ? 0 : 1;
}

bool game_is_computer_turn(const Game *g)
{
    return g->screen == SCREEN_PLAY && board_result(&g->board) == RESULT_NONE &&
           g->players[mover_index(g)] != PLAYER_HUMAN;
}

static void set_turn_message(Game *g)
{
    int mark = board_to_move(&g->board);
    const char *name = mark == MARK_X ? "X" : "O";
    if (game_is_computer_turn(g))
        (void)snprintf(g->message, sizeof g->message, "%s (%s) is thinking...", name,
                       player_name(g->players[mover_index(g)]));
    else
        (void)snprintf(g->message, sizeof g->message, "%s to move", name);
}

void game_new_round(Game *g)
{
    int first = MARK_X;
    if (g->starts == STARTS_ALTERNATE && g->games % 2 == 1) first = MARK_O;
    g->games++;
    board_init(&g->board, first);
    g->screen = SCREEN_PLAY;
    g->cursor = 4;
    g->hint = -1;
    g->last_result = RESULT_NONE;
    g->ai_wait_ms = AI_DELAY_MS;
    set_turn_message(g);
}

static void finish_if_over(Game *g)
{
    int result = board_result(&g->board);
    if (result == RESULT_NONE) {
        set_turn_message(g);
        return;
    }
    g->last_result = result;
    if (result == RESULT_X) g->wins_x++;
    else if (result == RESULT_O) g->wins_o++;
    else g->draws++;
    (void)snprintf(g->message, sizeof g->message, "%s",
                   result == RESULT_DRAW ? "It's a draw" :
                   result == RESULT_X ? "X wins" : "O wins");
    g->screen = SCREEN_OVER;
    g->over_row = OVER_AGAIN;
}

static void play_cell(Game *g, int cell)
{
    if (!board_play(&g->board, cell)) return;
    g->hint = -1;
    g->ai_wait_ms = AI_DELAY_MS;
    finish_if_over(g);
}

void game_update(Game *g, int elapsed_ms)
{
    if (!game_is_computer_turn(g)) return;
    g->ai_wait_ms -= elapsed_ms;
    if (g->ai_wait_ms > 0) return;
    int cell = choose_move(&g->board, g->players[mover_index(g)], g->level, &g->rng);
    if (cell >= 0) {
        g->cursor = cell;
        play_cell(g, cell);
    }
}

/* ---------- menus ---------- */

static bool menu_nav(const Input *in, int *row, int rows)
{
    switch (in->key) {
    case KEY_UP: case 'w': case 'W': case 'k':
        *row = (*row + rows - 1) % rows;
        return true;
    case KEY_DOWN: case 's': case 'S': case 'j':
        *row = (*row + 1) % rows;
        return true;
    default:
        return false;
    }
}

static bool confirm(int key)
{
    return key == KEY_ENTER || key == ' ';
}

static void change_menu_row(Game *g, int step)
{
    switch (g->menu_row) {
    case MENU_X: case MENU_O: {
        int side = g->menu_row == MENU_X ? 0 : 1;
        g->players[side] = (g->players[side] + PLAYER_COUNT + step) % PLAYER_COUNT;
        break;
    }
    case MENU_LEVEL:
        g->level = (g->level + LEVEL_COUNT + step) % LEVEL_COUNT;
        break;
    case MENU_STARTS:
        g->starts = (g->starts + STARTS_COUNT + step) % STARTS_COUNT;
        break;
    default:
        break;
    }
}

static void start_match(Game *g)
{
    g->games = g->wins_x = g->wins_o = g->draws = 0;
    game_new_round(g);
}

static void to_menu(Game *g)
{
    g->screen = SCREEN_MENU;
    g->menu_row = MENU_PLAY;
    g->message[0] = '\0';
}

static void menu_input(Game *g, const Input *in)
{
    if (in->key == KEY_MOUSE) {
        if (in->mouse_y < 0) return;          /* mouse_y carries the hit row */
        g->menu_row = in->mouse_y;
        if (g->menu_row == MENU_PLAY) start_match(g);
        else if (g->menu_row == MENU_QUIT) g->quit = true;
        else change_menu_row(g, 1);
        return;
    }
    if (menu_nav(in, &g->menu_row, MENU_ROWS)) return;
    switch (in->key) {
    case KEY_LEFT: case 'a': case 'A': case 'h': change_menu_row(g, -1); break;
    case KEY_RIGHT: case 'd': case 'D': case 'l': change_menu_row(g, 1); break;
    case KEY_ESC: g->menu_row = MENU_QUIT; break;   /* never quits by itself */
    default:
        if (!confirm(in->key)) break;
        if (g->menu_row == MENU_PLAY) start_match(g);
        else if (g->menu_row == MENU_QUIT) g->quit = true;
        else change_menu_row(g, 1);
        break;
    }
}

static void pause_choose(Game *g)
{
    switch (g->pause_row) {
    case PAUSE_RESUME:
        g->screen = SCREEN_PLAY;
        set_turn_message(g);
        break;
    case PAUSE_RESTART:
        g->games--;                  /* the same game again, same first player */
        game_new_round(g);
        break;
    case PAUSE_MENU: to_menu(g); break;
    default: g->quit = true; break;
    }
}

static void pause_input(Game *g, const Input *in)
{
    if (in->key == KEY_MOUSE) {
        if (in->mouse_y < 0) return;
        g->pause_row = in->mouse_y;
        pause_choose(g);
        return;
    }
    if (menu_nav(in, &g->pause_row, PAUSE_ROWS)) return;
    if (in->key == KEY_ESC || in->key == 'p' || in->key == 'P') {
        g->pause_row = PAUSE_RESUME;
        pause_choose(g);
    } else if (confirm(in->key)) {
        pause_choose(g);
    }
}

static void over_choose(Game *g)
{
    switch (g->over_row) {
    case OVER_AGAIN: game_new_round(g); break;
    case OVER_MENU:  to_menu(g); break;
    default:         g->quit = true; break;
    }
}

static void over_input(Game *g, const Input *in)
{
    if (in->key == KEY_MOUSE) {
        if (in->mouse_y < 0) return;
        g->over_row = in->mouse_y;
        over_choose(g);
        return;
    }
    if (menu_nav(in, &g->over_row, OVER_ROWS)) return;
    if (in->key == KEY_ESC) to_menu(g);
    else if (confirm(in->key)) over_choose(g);
}

/* ---------- play ---------- */

static void move_cursor(Game *g, int dx, int dy)
{
    int x = (g->cursor % 3 + dx + 3) % 3;
    int y = (g->cursor / 3 + dy + 3) % 3;
    g->cursor = y * 3 + x;
}

static void play_input(Game *g, const Input *in)
{
    switch (in->key) {
    case KEY_ESC: case 'p': case 'P':
        g->screen = SCREEN_PAUSE;
        g->pause_row = PAUSE_RESUME;
        return;
    case KEY_UP: case 'w': case 'W': case 'k': move_cursor(g, 0, -1); return;
    case KEY_DOWN: case 's': case 'S': case 'j': move_cursor(g, 0, 1); return;
    case KEY_LEFT: case 'a': case 'A': case 'h': move_cursor(g, -1, 0); return;
    case KEY_RIGHT: case 'd': case 'D': case 'l': move_cursor(g, 1, 0); return;
    case '?': {
        float scores[POLICY_ACTIONS];
        uint16_t legal = board_legal(&g->board);
        g->hint = -1;
        if (!game_is_computer_turn(g) && neural_scores(&g->board, scores))
            for (int cell = 0; cell < 9; cell++)
                if (legal >> cell & 1u && (g->hint < 0 || scores[cell] > scores[g->hint]))
                    g->hint = cell;
        if (g->hint >= 0) {
            g->cursor = g->hint;
            (void)snprintf(g->message, sizeof g->message,
                           "The network would play here");
        }
        return;
    }
    default:
        break;
    }
    if (game_is_computer_turn(g)) return;       /* wait for the computer */
    int cell = -1;
    if (in->key >= '1' && in->key <= '9') {
        /* Numpad layout: 7 8 9 is the top row. */
        int n = in->key - '1';
        cell = (2 - n / 3) * 3 + n % 3;
    } else if (confirm(in->key)) {
        cell = g->cursor;
    } else if (in->key == KEY_MOUSE) {
        cell = in->mouse_x;                 /* mouse_x carries the hit cell */
    }
    if (cell < 0) return;
    if (!(board_legal(&g->board) >> cell & 1u)) {
        (void)snprintf(g->message, sizeof g->message, "That square is taken");
        return;
    }
    g->cursor = cell;
    play_cell(g, cell);
}

void game_input(Game *g, const Input *in)
{
    if (!in || in->key == KEY_NONE) return;
    switch (g->screen) {
    case SCREEN_MENU:  menu_input(g, in); break;
    case SCREEN_PAUSE: pause_input(g, in); break;
    case SCREEN_OVER:  over_input(g, in); break;
    default:           play_input(g, in); break;
    }
}
