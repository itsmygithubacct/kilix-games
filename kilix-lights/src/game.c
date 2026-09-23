/* game.c - solvable puzzle generation, rules, undo, and layout. */
#include "game.h"

#include <string.h>

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static uint64_t board_mask(int size)
{
    int cells = size * size;
    return cells == 64 ? UINT64_MAX : ((UINT64_C(1) << cells) - 1u);
}

static int popcount64(uint64_t value)
{
    int count = 0;
    while (value != 0u) {
        value &= value - 1u;
        count++;
    }
    return count;
}

int game_mode_index(int size)
{
    return size == 3 ? 0 : (size == 5 ? 1 : 2);
}

const char *game_mode_name(int size)
{
    return size == 3 ? "STARTER" : (size == 5 ? "CLASSIC" : "EXPERT");
}

uint64_t game_toggle_mask(int size, int cell)
{
    uint64_t mask;
    int row;
    int col;
    if ((size != 3 && size != 5 && size != 7) ||
        cell < 0 || cell >= size * size)
        return 0u;
    row = cell / size;
    col = cell % size;
    mask = UINT64_C(1) << cell;
    if (row > 0) mask |= UINT64_C(1) << (cell - size);
    if (row + 1 < size) mask |= UINT64_C(1) << (cell + size);
    if (col > 0) mask |= UINT64_C(1) << (cell - 1);
    if (col + 1 < size) mask |= UINT64_C(1) << (cell + 1);
    return mask;
}

static void generate(Game *game)
{
    uint32_t state = game->seed ^ ((uint32_t)game->size * 0x9e3779b9u);
    uint64_t limit = board_mask(game->size);
    int cells = game->size * game->size;

    for (int attempt = 0; attempt < 256; attempt++) {
        uint64_t press = 0u;
        uint64_t board = 0u;
        for (int cell = 0; cell < cells; cell++) {
            uint32_t roll = next_random(&state);
            if ((roll & 7u) < 3u) press |= UINT64_C(1) << cell;
        }
        for (int cell = 0; cell < cells; cell++)
            if ((press & (UINT64_C(1) << cell)) != 0u)
                board ^= game_toggle_mask(game->size, cell);
        board &= limit;
        if (board != 0u && popcount64(board) >= game->size &&
            popcount64(board) <= cells - 2) {
            game->generation_mask = press;
            game->board = board;
            game->initial = board;
            return;
        }
    }

    /* Deterministic fallback. It is constructed by legal presses too. */
    game->generation_mask = UINT64_C(1) | (UINT64_C(1) << (cells / 2));
    game->board = game_toggle_mask(game->size, 0) ^
                  game_toggle_mask(game->size, cells / 2);
    game->initial = game->board;
}

void game_init(Game *game)
{
    memset(game, 0, sizeof *game);
    game->size = 5;
    game->seed = UINT32_C(0x4c494748);
    game->puzzle_number = 1;
    game->focus_cell = 12;
    game->hover_cell = -1;
    game->pressed_cell = -1;
    for (int i = 0; i < 3; i++) game->best[i] = -1;
    generate(game);
}

void game_new(Game *game)
{
    game->seed = game->seed * 1664525u + 1013904223u;
    game->puzzle_number++;
    game->moves = 0;
    game->history_len = 0;
    game->won = false;
    game->win_time = 0.0;
    memset(game->cell_anim, 0, sizeof game->cell_anim);
    generate(game);
}

void game_reset(Game *game)
{
    game->board = game->initial;
    game->moves = 0;
    game->history_len = 0;
    game->won = false;
    game->win_time = 0.0;
    game->pressed_cell = -1;
    memset(game->cell_anim, 0, sizeof game->cell_anim);
}

bool game_undo(Game *game)
{
    if (game->history_len <= 0) return false;
    game->board = game->history[--game->history_len];
    if (game->moves > 0) game->moves--;
    game->won = false;
    game->win_time = 0.0;
    return true;
}

void game_set_size(Game *game, int size)
{
    if (size != 3 && size != 5 && size != 7) return;
    if (game->size == size) {
        game_new(game);
        return;
    }
    game->size = size;
    game->focus_cell = size * size / 2;
    game->hover_cell = -1;
    game->pressed_cell = -1;
    game->seed ^= (uint32_t)size * UINT32_C(0x45d9f3b);
    game_new(game);
}

bool game_activate(Game *game, int cell, bool *ended_on)
{
    int cells = game->size * game->size;
    if (game->won || cell < 0 || cell >= cells) return false;
    if (game->history_len == (int)(sizeof game->history /
                                   sizeof game->history[0])) {
        memmove(game->history, game->history + 1,
                (sizeof game->history) - sizeof game->history[0]);
        game->history_len--;
    }
    game->history[game->history_len++] = game->board;
    game->board ^= game_toggle_mask(game->size, cell);
    game->moves++;
    game->focus_cell = cell;
    game->cell_anim[cell] = 1.0;
    if (ended_on != NULL)
        *ended_on = (game->board & (UINT64_C(1) << cell)) != 0u;
    if (game->board == 0u) {
        int mode = game_mode_index(game->size);
        game->won = true;
        game->win_time = 0.0;
        if (game->best[mode] < 0 || game->moves < game->best[mode])
            game->best[mode] = game->moves;
    }
    return true;
}

bool game_cell_on(const Game *game, int cell)
{
    return cell >= 0 && cell < game->size * game->size &&
           (game->board & (UINT64_C(1) << cell)) != 0u;
}

int game_lit_count(const Game *game)
{
    return popcount64(game->board);
}

GameRect game_cell_rect(const Game *game, int cell)
{
    int size = game->size == 3 ? 68 : (game->size == 5 ? 46 : 32);
    int gap = game->size == 3 ? 10 : (game->size == 5 ? 7 : 6);
    int span = game->size * size + (game->size - 1) * gap;
    int left = 240 - span / 2;
    int top = 212 - span / 2;
    int row = cell / game->size;
    int col = cell % game->size;
    GameRect rect = {left + col * (size + gap),
                     top + row * (size + gap), size, size};
    return rect;
}

int game_cell_from_rect(const Game *game, int x, int y)
{
    for (int cell = 0; cell < game->size * game->size; cell++) {
        GameRect r = game_cell_rect(game, cell);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return cell;
    }
    return -1;
}

void game_update(Game *game, double dt)
{
    game->pulse += dt;
    if (game->pulse > 1000.0) game->pulse -= 1000.0;
    if (game->won) game->win_time += dt;
    for (int i = 0; i < MAX_CELLS; i++) {
        game->cell_anim[i] -= dt * 7.0;
        if (game->cell_anim[i] < 0.0) game->cell_anim[i] = 0.0;
    }
}

bool game_rules_selftest(void)
{
    Game a;
    Game b;
    uint64_t original;
    uint64_t replay;
    bool ended_on = false;

    if (popcount64(game_toggle_mask(5, 0)) != 3 ||
        popcount64(game_toggle_mask(5, 2)) != 4 ||
        popcount64(game_toggle_mask(5, 12)) != 5)
        return false;
    if ((game_toggle_mask(5, 4) & (UINT64_C(1) << 5)) != 0u)
        return false;

    game_init(&a);
    b = a;
    if (a.board == 0u || a.board != b.board ||
        a.generation_mask != b.generation_mask)
        return false;
    replay = a.board;
    for (int i = 0; i < a.size * a.size; i++)
        if ((a.generation_mask & (UINT64_C(1) << i)) != 0u)
            replay ^= game_toggle_mask(a.size, i);
    if (replay != 0u) return false;

    original = a.board;
    if (!game_activate(&a, 12, &ended_on) ||
        !game_activate(&a, 12, &ended_on) || a.board != original)
        return false;
    if (!game_undo(&a) || a.board == original || !game_undo(&a) ||
        a.board != original || a.moves != 0)
        return false;
    game_activate(&a, 0, &ended_on);
    game_reset(&a);
    if (a.board != a.initial || a.moves != 0 || a.history_len != 0)
        return false;
    return true;
}
