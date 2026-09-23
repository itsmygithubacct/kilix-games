#include "chess_bash.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GameState G;

static uint32_t rng_state = 0x5eed1234u;

/* position hashes since the last irreversible move, for threefold detection */
static uint64_t pos_hashes[MAX_PLIES];
static int pos_hash_count;

static const int diff_movetime[DIFF_COUNT] = { 120, 450, 1500 };

/* Search and legality checks only mutate the chess position.  Keeping this
 * compact avoids copying the 32 KiB UCI history at every search node. */
typedef struct {
    char board[64];
    int side;
    bool castle_wk, castle_wq, castle_bk, castle_bq;
    int ep_square;
    int halfmove_clock;
    int fullmove_number;
} PositionSnapshot;

static void position_save(PositionSnapshot *p)
{
    memcpy(p->board, G.board, sizeof p->board);
    p->side = G.side;
    p->castle_wk = G.castle_wk;
    p->castle_wq = G.castle_wq;
    p->castle_bk = G.castle_bk;
    p->castle_bq = G.castle_bq;
    p->ep_square = G.ep_square;
    p->halfmove_clock = G.halfmove_clock;
    p->fullmove_number = G.fullmove_number;
}

static void position_restore(const PositionSnapshot *p)
{
    memcpy(G.board, p->board, sizeof p->board);
    G.side = p->side;
    G.castle_wk = p->castle_wk;
    G.castle_wq = p->castle_wq;
    G.castle_bk = p->castle_bk;
    G.castle_bq = p->castle_bq;
    G.ep_square = p->ep_square;
    G.halfmove_clock = p->halfmove_clock;
    G.fullmove_number = p->fullmove_number;
}

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t xrng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int sq_file(int sq) { return sq & 7; }
int sq_rank(int sq) { return sq >> 3; }
int make_sq(int file, int rank) { return rank * 8 + file; }

const char *side_name(int side) { return side == SIDE_WHITE ? "Red" : "Blue"; }

const char *mode_name(int mode)
{
    switch (mode) {
    case MODE_HUMAN_HUMAN: return "Human vs Human";
    case MODE_HUMAN_AI: return "Human vs Computer";
    case MODE_AI_AI: return "Computer vs Computer";
    default: return "Unknown";
    }
}

const char *difficulty_name(int diff)
{
    switch (diff) {
    case 0: return "Squire";
    case 1: return "Knight";
    case 2: return "Warlord";
    default: return "Knight";
    }
}

char piece_upper(char p) { return (char)toupper((unsigned char)p); }

int piece_color(char p)
{
    if (p >= 'A' && p <= 'Z') return SIDE_WHITE;
    if (p >= 'a' && p <= 'z') return SIDE_BLACK;
    return -1;
}

int piece_type(char p)
{
    switch (piece_upper(p)) {
    case 'P': return PT_PAWN;
    case 'N': return PT_KNIGHT;
    case 'B': return PT_BISHOP;
    case 'R': return PT_ROOK;
    case 'Q': return PT_QUEEN;
    case 'K': return PT_KING;
    default: return -1;
    }
}

const char *piece_name(int t)
{
    static const char *names[PT_COUNT] = {
        "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"
    };
    return (t >= 0 && t < PT_COUNT) ? names[t] : "Piece";
}

static void square_name(int sq, char out[3])
{
    if (sq < 0 || sq >= 64) {
        snprintf(out, 3, "??");
        return;
    }
    out[0] = (char)('a' + sq_file(sq));
    out[1] = (char)('1' + sq_rank(sq));
    out[2] = '\0';
}

static bool on_board(int f, int r)
{
    return f >= 0 && f < 8 && r >= 0 && r < 8;
}

static bool is_empty_at(const GameState *s, int sq)
{
    return sq >= 0 && sq < 64 && s->board[sq] == '.';
}

static bool enemy_at(const GameState *s, int sq, int side)
{
    return sq >= 0 && sq < 64 && piece_color(s->board[sq]) == 1 - side &&
           piece_type(s->board[sq]) != PT_KING;
}

static void add_move(Move *moves, int *count, Move m)
{
    if (*count < MAX_MOVES)
        moves[(*count)++] = m;
}

static bool square_attacked(const GameState *s, int sq, int by_side)
{
    int f = sq_file(sq);
    int r = sq_rank(sq);
    char pawn = by_side == SIDE_WHITE ? 'P' : 'p';
    char knight = by_side == SIDE_WHITE ? 'N' : 'n';
    char bishop = by_side == SIDE_WHITE ? 'B' : 'b';
    char rook = by_side == SIDE_WHITE ? 'R' : 'r';
    char queen = by_side == SIDE_WHITE ? 'Q' : 'q';
    char king = by_side == SIDE_WHITE ? 'K' : 'k';

    int pawn_dir = by_side == SIDE_WHITE ? 1 : -1;
    for (int df = -1; df <= 1; df += 2) {
        int pf = f - df;
        int pr = r - pawn_dir;
        if (on_board(pf, pr) && s->board[make_sq(pf, pr)] == pawn)
            return true;
    }

    static const int knight_d[8][2] = {
        { 1, 2 }, { 2, 1 }, { 2,-1 }, { 1,-2 },
        {-1,-2 }, {-2,-1 }, {-2, 1 }, {-1, 2 }
    };
    for (int i = 0; i < 8; i++) {
        int nf = f + knight_d[i][0];
        int nr = r + knight_d[i][1];
        if (on_board(nf, nr) && s->board[make_sq(nf, nr)] == knight)
            return true;
    }

    static const int diag_d[4][2] = { {1,1}, {1,-1}, {-1,1}, {-1,-1} };
    for (int i = 0; i < 4; i++) {
        int nf = f + diag_d[i][0], nr = r + diag_d[i][1];
        while (on_board(nf, nr)) {
            char p = s->board[make_sq(nf, nr)];
            if (p != '.') {
                if (p == bishop || p == queen) return true;
                break;
            }
            nf += diag_d[i][0];
            nr += diag_d[i][1];
        }
    }

    static const int line_d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    for (int i = 0; i < 4; i++) {
        int nf = f + line_d[i][0], nr = r + line_d[i][1];
        while (on_board(nf, nr)) {
            char p = s->board[make_sq(nf, nr)];
            if (p != '.') {
                if (p == rook || p == queen) return true;
                break;
            }
            nf += line_d[i][0];
            nr += line_d[i][1];
        }
    }

    for (int dr = -1; dr <= 1; dr++) {
        for (int df = -1; df <= 1; df++) {
            if (!df && !dr) continue;
            int kf = f + df, kr = r + dr;
            if (on_board(kf, kr) && s->board[make_sq(kf, kr)] == king)
                return true;
        }
    }
    return false;
}

int game_king_square(int side)
{
    char king = side == SIDE_WHITE ? 'K' : 'k';
    for (int i = 0; i < 64; i++)
        if (G.board[i] == king) return i;
    return -1;
}

static int king_square_state(const GameState *s, int side)
{
    char king = side == SIDE_WHITE ? 'K' : 'k';
    for (int i = 0; i < 64; i++)
        if (s->board[i] == king) return i;
    return -1;
}

bool game_side_in_check(int side)
{
    int k = game_king_square(side);
    return k >= 0 && square_attacked(&G, k, 1 - side);
}

static void apply_move_state(GameState *s, Move m)
{
    char moved = s->board[m.from];
    bool moved_pawn = piece_upper(moved) == 'P';
    int side = piece_color(moved);
    char captured = m.en_passant
        ? s->board[make_sq(sq_file(m.to), sq_rank(m.from))]
        : s->board[m.to];

    s->board[m.from] = '.';
    if (m.en_passant)
        s->board[make_sq(sq_file(m.to), sq_rank(m.from))] = '.';

    if (m.castle) {
        if (m.to == make_sq(6, 0)) {
            s->board[make_sq(5, 0)] = s->board[make_sq(7, 0)];
            s->board[make_sq(7, 0)] = '.';
        } else if (m.to == make_sq(2, 0)) {
            s->board[make_sq(3, 0)] = s->board[make_sq(0, 0)];
            s->board[make_sq(0, 0)] = '.';
        } else if (m.to == make_sq(6, 7)) {
            s->board[make_sq(5, 7)] = s->board[make_sq(7, 7)];
            s->board[make_sq(7, 7)] = '.';
        } else if (m.to == make_sq(2, 7)) {
            s->board[make_sq(3, 7)] = s->board[make_sq(0, 7)];
            s->board[make_sq(0, 7)] = '.';
        }
    }

    if (m.promo) {
        char pc = (char)toupper((unsigned char)m.promo);
        moved = side == SIDE_WHITE ? pc : (char)tolower((unsigned char)pc);
    }
    s->board[m.to] = moved;

    if (m.from == make_sq(4, 0) || moved == 'K') {
        s->castle_wk = false;
        s->castle_wq = false;
    }
    if (m.from == make_sq(4, 7) || moved == 'k') {
        s->castle_bk = false;
        s->castle_bq = false;
    }
    if (m.from == make_sq(0, 0) || (m.to == make_sq(0, 0) && captured == 'R'))
        s->castle_wq = false;
    if (m.from == make_sq(7, 0) || (m.to == make_sq(7, 0) && captured == 'R'))
        s->castle_wk = false;
    if (m.from == make_sq(0, 7) || (m.to == make_sq(0, 7) && captured == 'r'))
        s->castle_bq = false;
    if (m.from == make_sq(7, 7) || (m.to == make_sq(7, 7) && captured == 'r'))
        s->castle_bk = false;

    s->ep_square = -1;
    if (piece_upper(s->board[m.to]) == 'P' && abs(m.to - m.from) == 16)
        s->ep_square = (m.from + m.to) / 2;

    if (moved_pawn || captured != '.')
        s->halfmove_clock = 0;
    else
        s->halfmove_clock++;
    if (side == SIDE_BLACK)
        s->fullmove_number++;
    s->side = 1 - s->side;
}

static bool legal_after_move(Move m, int moving_side)
{
    PositionSnapshot saved;
    position_save(&saved);
    apply_move_state(&G, m);
    int k = king_square_state(&G, moving_side);
    bool legal = k >= 0 && !square_attacked(&G, k, 1 - moving_side);
    position_restore(&saved);
    return legal;
}

static void add_pawn_push(Move *moves, int *count, int from, int to,
                          bool capture, bool ep)
{
    int rank = sq_rank(to);
    bool promote = rank == 0 || rank == 7;
    if (promote) {
        static const char promos[] = { 'q', 'r', 'b', 'n' };
        for (int i = 0; i < 4; i++)
            add_move(moves, count, (Move){ from, to, promos[i], capture, false, ep, false });
    } else {
        add_move(moves, count, (Move){ from, to, 0, capture, false, ep, false });
    }
}

static int generate_pseudo(Move *moves)
{
    int count = 0;
    int side = G.side;
    for (int sq = 0; sq < 64; sq++) {
        char p = G.board[sq];
        if (piece_color(p) != side) continue;
        int f = sq_file(sq), r = sq_rank(sq);
        int type = piece_type(p);

        if (type == PT_PAWN) {
            int dir = side == SIDE_WHITE ? 1 : -1;
            int start = side == SIDE_WHITE ? 1 : 6;
            int nr = r + dir;
            if (on_board(f, nr) && is_empty_at(&G, make_sq(f, nr))) {
                add_pawn_push(moves, &count, sq, make_sq(f, nr), false, false);
                if (r == start && is_empty_at(&G, make_sq(f, r + 2 * dir)))
                    add_move(moves, &count, (Move){ sq, make_sq(f, r + 2 * dir), 0, false, false, false, true });
            }
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (!on_board(nf, nr)) continue;
                int to = make_sq(nf, nr);
                if (enemy_at(&G, to, side))
                    add_pawn_push(moves, &count, sq, to, true, false);
                else if (to == G.ep_square)
                    add_pawn_push(moves, &count, sq, to, true, true);
            }
        } else if (type == PT_KNIGHT) {
            static const int d[8][2] = {
                {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}
            };
            for (int i = 0; i < 8; i++) {
                int nf = f + d[i][0], nr = r + d[i][1];
                if (!on_board(nf, nr)) continue;
                int to = make_sq(nf, nr);
                int c = piece_color(G.board[to]);
                if (c == side) continue;
                if (c != 1 - side || piece_type(G.board[to]) != PT_KING)
                    add_move(moves, &count, (Move){ sq, to, 0, c == 1 - side, false, false, false });
            }
        } else if (type == PT_BISHOP || type == PT_ROOK || type == PT_QUEEN) {
            static const int dirs[8][2] = {
                {1,1},{1,-1},{-1,1},{-1,-1},{1,0},{-1,0},{0,1},{0,-1}
            };
            int first = type == PT_BISHOP ? 0 : type == PT_ROOK ? 4 : 0;
            int last = type == PT_BISHOP ? 4 : type == PT_ROOK ? 8 : 8;
            for (int i = first; i < last; i++) {
                int nf = f + dirs[i][0], nr = r + dirs[i][1];
                while (on_board(nf, nr)) {
                    int to = make_sq(nf, nr);
                    int c = piece_color(G.board[to]);
                    if (c == side) break;
                    if (c != 1 - side || piece_type(G.board[to]) != PT_KING)
                        add_move(moves, &count, (Move){ sq, to, 0, c == 1 - side, false, false, false });
                    if (c == 1 - side) break;
                    nf += dirs[i][0];
                    nr += dirs[i][1];
                }
            }
        } else if (type == PT_KING) {
            for (int dr = -1; dr <= 1; dr++) {
                for (int df = -1; df <= 1; df++) {
                    if (!df && !dr) continue;
                    int nf = f + df, nr = r + dr;
                    if (!on_board(nf, nr)) continue;
                    int to = make_sq(nf, nr);
                    int c = piece_color(G.board[to]);
                    if (c == side) continue;
                    if (c != 1 - side || piece_type(G.board[to]) != PT_KING)
                        add_move(moves, &count, (Move){ sq, to, 0, c == 1 - side, false, false, false });
                }
            }
            if (side == SIDE_WHITE && sq == make_sq(4, 0) &&
                !square_attacked(&G, sq, SIDE_BLACK)) {
                if (G.castle_wk && G.board[make_sq(7,0)] == 'R' &&
                    G.board[make_sq(5,0)] == '.' && G.board[make_sq(6,0)] == '.' &&
                    !square_attacked(&G, make_sq(5,0), SIDE_BLACK) &&
                    !square_attacked(&G, make_sq(6,0), SIDE_BLACK))
                    add_move(moves, &count, (Move){ sq, make_sq(6,0), 0, false, true, false, false });
                if (G.castle_wq && G.board[make_sq(0,0)] == 'R' &&
                    G.board[make_sq(3,0)] == '.' && G.board[make_sq(2,0)] == '.' &&
                    G.board[make_sq(1,0)] == '.' &&
                    !square_attacked(&G, make_sq(3,0), SIDE_BLACK) &&
                    !square_attacked(&G, make_sq(2,0), SIDE_BLACK))
                    add_move(moves, &count, (Move){ sq, make_sq(2,0), 0, false, true, false, false });
            } else if (side == SIDE_BLACK && sq == make_sq(4, 7) &&
                       !square_attacked(&G, sq, SIDE_WHITE)) {
                if (G.castle_bk && G.board[make_sq(7,7)] == 'r' &&
                    G.board[make_sq(5,7)] == '.' && G.board[make_sq(6,7)] == '.' &&
                    !square_attacked(&G, make_sq(5,7), SIDE_WHITE) &&
                    !square_attacked(&G, make_sq(6,7), SIDE_WHITE))
                    add_move(moves, &count, (Move){ sq, make_sq(6,7), 0, false, true, false, false });
                if (G.castle_bq && G.board[make_sq(0,7)] == 'r' &&
                    G.board[make_sq(3,7)] == '.' && G.board[make_sq(2,7)] == '.' &&
                    G.board[make_sq(1,7)] == '.' &&
                    !square_attacked(&G, make_sq(3,7), SIDE_WHITE) &&
                    !square_attacked(&G, make_sq(2,7), SIDE_WHITE))
                    add_move(moves, &count, (Move){ sq, make_sq(2,7), 0, false, true, false, false });
            }
        }
    }
    return count;
}

void game_generate_legal(void)
{
    Move pseudo[MAX_MOVES];
    int n = generate_pseudo(pseudo);
    G.legal_count = 0;
    int moving_side = G.side;
    for (int i = 0; i < n; i++) {
        if (legal_after_move(pseudo[i], moving_side))
            G.legal[G.legal_count++] = pseudo[i];
    }
}

static int battle_effect_for(int attacker, int victim)
{
    static const int table[PT_COUNT][PT_COUNT] = {
        { 0, 11,  0, 12,  1,  4 },
        {10,  1,  6, 10,  1,  4 },
        { 8,  8,  3,  8,  9,  4 },
        { 2,  7,  2,  2,  7,  4 },
        { 9,  3,  9,  9,  9,  4 },
        { 1,  1,  4,  2,  4,  4 },
    };
    if (attacker < 0 || attacker >= PT_COUNT || victim < 0 || victim >= PT_COUNT)
        return -1;
    return table[attacker][victim];
}

static const char *battle_verb(int attacker, int victim)
{
    static const char *verbs[PT_COUNT][PT_COUNT] = {
        { "jabs", "trips", "pokes", "chips", "ambushes", "storms" },
        { "charges", "duels", "rides down", "circles", "lances", "checks" },
        { "blesses", "sears", "banishes", "cracks", "exorcises", "judges" },
        { "flattens", "walls off", "crushes", "collides with", "swallows", "topples" },
        { "zaps", "charms", "unmakes", "shatters", "outspells", "crowns" },
        { "cuts down", "commands", "sentences", "orders", "outlaws", "confronts" },
    };
    if (attacker < 0 || attacker >= PT_COUNT || victim < 0 || victim >= PT_COUNT)
        return "takes";
    return verbs[attacker][victim];
}

static void start_animation(Move m)
{
    memset(&G.anim, 0, sizeof G.anim);
    G.anim.active = true;
    G.anim.move = m;
    G.anim.attacker = G.board[m.from];
    G.anim.victim_sq = m.en_passant
        ? make_sq(sq_file(m.to), sq_rank(m.from))
        : m.to;
    G.anim.victim = m.capture ? G.board[G.anim.victim_sq] : '.';
    G.anim.attacker_type = piece_type(G.anim.attacker);
    G.anim.victim_type = piece_type(G.anim.victim);
    G.anim.effect = battle_effect_for(G.anim.attacker_type, G.anim.victim_type);
    G.anim.rook_from = G.anim.rook_to = -1;
    if (m.castle) {
        int r = sq_rank(m.to);
        bool kingside = sq_file(m.to) == 6;
        G.anim.rook_from = make_sq(kingside ? 7 : 0, r);
        G.anim.rook_to = make_sq(kingside ? 5 : 3, r);
    }

    int dist = abs(sq_file(m.to) - sq_file(m.from));
    int dr = abs(sq_rank(m.to) - sq_rank(m.from));
    if (dr > dist) dist = dr;
    if (m.capture)
        G.anim.duration = 2.05f;
    else if (m.castle)
        G.anim.duration = 0.95f;
    else
        G.anim.duration = 0.34f + 0.11f * dist;
    if (G.fast_anim)
        G.anim.duration = m.capture ? 0.24f : 0.14f;
    G.anim.t = 0;
    G.resign_arm = 0;
    G.leave_arm = 0;

    if (m.capture) {
        snprintf(G.anim.caption, sizeof G.anim.caption, "%s %s %s",
                 piece_name(G.anim.attacker_type),
                 battle_verb(G.anim.attacker_type, G.anim.victim_type),
                 piece_name(G.anim.victim_type));
    } else if (m.castle) {
        snprintf(G.anim.caption, sizeof G.anim.caption, "%s castles %s",
                 side_name(piece_color(G.anim.attacker)),
                 sq_file(m.to) == 6 ? "short" : "long");
    } else if (m.promo) {
        snprintf(G.anim.caption, sizeof G.anim.caption, "Pawn ascends");
    } else {
        snprintf(G.anim.caption, sizeof G.anim.caption, "%s marches",
                 piece_name(G.anim.attacker_type));
    }
    G.state = GS_ANIMATING;
}

void game_move_to_uci(Move m, char out[8])
{
    out[0] = (char)('a' + sq_file(m.from));
    out[1] = (char)('1' + sq_rank(m.from));
    out[2] = (char)('a' + sq_file(m.to));
    out[3] = (char)('1' + sq_rank(m.to));
    if (m.promo) {
        out[4] = (char)tolower((unsigned char)m.promo);
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

static void append_history(const char *uci)
{
    size_t len = strlen(G.uci_history);
    size_t need = strlen(uci) + (len ? 1 : 0) + 1;
    if (len + need >= sizeof G.uci_history) return;
    if (len) strcat(G.uci_history, " ");
    strcat(G.uci_history, uci);
    snprintf(G.last_move, sizeof G.last_move, "%s", uci);
    G.ply_count++;
}

/* the ep square only distinguishes positions when a pawn could actually
 * take it (FIDE 9.2); a phantom ep after any double push would make the
 * first occurrence hash differently and delay repetition draws */
static bool ep_capture_possible(void)
{
    if (G.ep_square < 0) return false;
    int f = sq_file(G.ep_square);
    int r = sq_rank(G.ep_square);
    char pawn = G.side == SIDE_WHITE ? 'P' : 'p';
    int pr = G.side == SIDE_WHITE ? r - 1 : r + 1;
    for (int df = -1; df <= 1; df += 2) {
        int nf = f + df;
        if (!on_board(nf, pr)) continue;
        int from = make_sq(nf, pr);
        if (G.board[from] != pawn) continue;
        /* FIDE repetition identity includes an en-passant square only when
         * the capture is genuinely legal.  An adjacent pawn pinned to its
         * king cannot exercise the right and must not split the hash. */
        Move ep = { from, G.ep_square, 0, true, false, true, false };
        if (legal_after_move(ep, G.side))
            return true;
    }
    return false;
}

static uint64_t position_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 64; i++) {
        h ^= (uint8_t)G.board[i];
        h *= 0x100000001b3ull;
    }
    uint8_t tail[6] = {
        (uint8_t)G.side,
        G.castle_wk, G.castle_wq, G.castle_bk, G.castle_bq,
        (uint8_t)(ep_capture_possible() ? G.ep_square + 1 : 0)
    };
    for (int i = 0; i < 6; i++) {
        h ^= tail[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void push_position_hash(bool irreversible)
{
    /* an irreversible move (pawn push, capture, castle-rights change) means
     * earlier positions can never recur; restart the window */
    if (irreversible)
        pos_hash_count = 0;
    if (pos_hash_count < MAX_PLIES)
        pos_hashes[pos_hash_count++] = position_hash();
}

static int repetition_count(void)
{
    if (!pos_hash_count) return 0;
    uint64_t cur = pos_hashes[pos_hash_count - 1];
    int n = 0;
    for (int i = 0; i < pos_hash_count; i++)
        if (pos_hashes[i] == cur) n++;
    return n;
}

static bool insufficient_material(void)
{
    int knights = 0, bishops[2] = {0, 0}; /* bishops by square color */
    for (int sq = 0; sq < 64; sq++) {
        switch (piece_upper(G.board[sq])) {
        case '.': case 'K': break;
        case 'N': knights++; break;
        case 'B': bishops[(sq_file(sq) + sq_rank(sq)) & 1]++; break;
        default: return false;   /* pawn, rook or queen: mate is possible */
        }
    }
    int minors = knights + bishops[0] + bishops[1];
    if (minors <= 1) return true;                       /* K, KN, KB vs K */
    if (!knights && (!bishops[0] || !bishops[1]))
        return true;                                    /* same-color bishops */
    return false;
}

static void game_over(int kind, int winner)
{
    G.result_kind = kind;
    G.winner_side = winner;
    G.over_t = 0;
    G.loser_king_sq = winner >= 0 ? game_king_square(1 - winner) : -1;
    switch (kind) {
    case RESULT_CHECKMATE:
        snprintf(G.result, sizeof G.result, "Checkmate - %s wins", side_name(winner));
        break;
    case RESULT_STALEMATE:
        snprintf(G.result, sizeof G.result, "Stalemate - draw");
        break;
    case RESULT_FIFTY_MOVE:
        snprintf(G.result, sizeof G.result, "Draw - fifty quiet moves");
        break;
    case RESULT_REPETITION:
        snprintf(G.result, sizeof G.result, "Draw - threefold repetition");
        break;
    case RESULT_MATERIAL:
        snprintf(G.result, sizeof G.result, "Draw - bare armies");
        break;
    case RESULT_RESIGN:
        snprintf(G.result, sizeof G.result, "%s resigns - %s wins",
                 side_name(1 - winner), side_name(winner));
        break;
    default:
        snprintf(G.result, sizeof G.result, "Game over");
        break;
    }
    snprintf(G.status, sizeof G.status, "%s", G.result);
    G.state = GS_GAMEOVER;
    if (winner >= 0) {
        sound_play(SND_WIN_TRUMPET, 0.95f, 1.0f);
        if (G.music_on)
            sound_music_play(MUS_VICTORY, 0.8f, false);
    } else {
        sound_play(SND_WIN_TRUMPET, 0.55f, 0.82f);
        if (G.music_on)
            sound_music_stop(1.2f);
    }
}

static void set_turn_status(void)
{
    G.check_flash = 0;
    if (game_side_in_check(G.side)) {
        snprintf(G.status, sizeof G.status, "%s to move - CHECK", side_name(G.side));
        G.check_sq = game_king_square(G.side);
    } else {
        snprintf(G.status, sizeof G.status, "%s to move", side_name(G.side));
        G.check_sq = -1;
    }
}

/* Load a complete chess position for headless rule/perft tests.  Parsing into
 * locals first keeps a rejected FEN from partially corrupting the live game. */
bool game_load_fen(const char *fen)
{
    if (!fen) return false;

    char board[64];
    memset(board, '.', sizeof board);
    int rank = 7, file = 0;
    int white_kings = 0, black_kings = 0;
    const char *p = fen;
    while (*p && *p != ' ') {
        unsigned char c = (unsigned char)*p++;
        if (c == '/') {
            if (file != 8 || rank <= 0) return false;
            rank--;
            file = 0;
            continue;
        }
        if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) return false;
            continue;
        }
        if (!strchr("PNBRQKpnbrqk", c) || file >= 8) return false;
        board[make_sq(file++, rank)] = (char)c;
        if (c == 'K') white_kings++;
        if (c == 'k') black_kings++;
    }
    if (rank != 0 || file != 8 || *p++ != ' ' ||
        white_kings != 1 || black_kings != 1)
        return false;

    int side;
    if (*p == 'w') side = SIDE_WHITE;
    else if (*p == 'b') side = SIDE_BLACK;
    else return false;
    p++;
    if (*p++ != ' ') return false;

    bool wk = false, wq = false, bk = false, bq = false;
    if (*p == '-') {
        p++;
    } else {
        int rights = 0;
        while (*p && *p != ' ') {
            unsigned bit;
            switch (*p++) {
            case 'K': bit = 1u; wk = true; break;
            case 'Q': bit = 2u; wq = true; break;
            case 'k': bit = 4u; bk = true; break;
            case 'q': bit = 8u; bq = true; break;
            default: return false;
            }
            if (rights & (int)bit) return false;
            rights |= (int)bit;
        }
    }
    if (*p++ != ' ') return false;

    int ep = -1;
    if (*p == '-') {
        p++;
    } else {
        if (p[0] < 'a' || p[0] > 'h' ||
            (p[1] != '3' && p[1] != '6'))
            return false;
        ep = make_sq(p[0] - 'a', p[1] - '1');
        p += 2;
    }
    if (*p++ != ' ') return false;

    char *end;
    long halfmove = strtol(p, &end, 10);
    if (end == p || halfmove < 0 || halfmove > 1000000 || *end != ' ')
        return false;
    p = end + 1;
    long fullmove = strtol(p, &end, 10);
    if (end == p || fullmove < 1 || fullmove > 1000000) return false;
    while (*end == ' ') end++;
    if (*end) return false;

    memcpy(G.board, board, sizeof G.board);
    G.side = side;
    G.castle_wk = wk;
    G.castle_wq = wq;
    G.castle_bk = bk;
    G.castle_bq = bq;
    G.ep_square = ep;
    G.halfmove_clock = (int)halfmove;
    G.fullmove_number = (int)fullmove;
    G.selected = -1;
    G.anim.active = false;
    G.uci_history[0] = '\0';
    G.last_move[0] = '\0';
    G.ply_count = 0;
    G.result[0] = '\0';
    G.result_kind = RESULT_NONE;
    G.winner_side = -1;
    G.loser_king_sq = -1;
    G.over_t = 0;
    G.check_flash = 0;
    G.check_sq = -1;
    G.resign_arm = G.leave_arm = 0;
    memset(G.captured, 0, sizeof G.captured);
    G.captured_count[0] = G.captured_count[1] = 0;
    pos_hash_count = 0;
    push_position_hash(false);
    game_generate_legal();
    set_turn_status();
    G.state = GS_PLAYING;
    return true;
}

static uint64_t perft_nodes(int depth)
{
    if (depth == 0) return 1;
    game_generate_legal();
    if (depth == 1) return (uint64_t)G.legal_count;

    Move moves[MAX_MOVES];
    int count = G.legal_count;
    memcpy(moves, G.legal, (size_t)count * sizeof *moves);
    PositionSnapshot saved;
    position_save(&saved);
    uint64_t nodes = 0;
    for (int i = 0; i < count; i++) {
        apply_move_state(&G, moves[i]);
        nodes += perft_nodes(depth - 1);
        position_restore(&saved);
    }
    return nodes;
}

uint64_t game_perft(int depth)
{
    if (depth < 0) return 0;
    PositionSnapshot saved;
    position_save(&saved);
    uint64_t nodes = perft_nodes(depth);
    position_restore(&saved);
    game_generate_legal();
    return nodes;
}

static void finish_animation(void)
{
    Move m = G.anim.move;
    char uci[8];
    game_move_to_uci(m, uci);
    append_history(uci);

    if (m.capture && G.anim.victim != '.') {
        int capturer = piece_color(G.anim.attacker);
        if (capturer >= 0 && G.captured_count[capturer] < 16)
            G.captured[capturer][G.captured_count[capturer]++] = G.anim.victim;
    }

    bool irreversible = m.capture || m.castle || m.promo ||
                        piece_type(G.anim.attacker) == PT_PAWN;
    apply_move_state(&G, m);
    push_position_hash(irreversible);
    G.selected = -1;
    G.anim.active = false;
    game_generate_legal();

    if (G.legal_count == 0) {
        if (game_side_in_check(G.side))
            game_over(RESULT_CHECKMATE, 1 - G.side);
        else
            game_over(RESULT_STALEMATE, -1);
        return;
    }
    if (G.halfmove_clock >= 100) {
        game_over(RESULT_FIFTY_MOVE, -1);
        return;
    }
    if (repetition_count() >= 3) {
        game_over(RESULT_REPETITION, -1);
        return;
    }
    if (insufficient_material()) {
        game_over(RESULT_MATERIAL, -1);
        return;
    }

    if (game_side_in_check(G.side)) {
        set_turn_status();
        G.check_flash = 1.4f;
        sound_play(SND_CHECK, 0.85f, 1.0f);
    } else {
        set_turn_status();
    }
    G.state = GS_PLAYING;
    G.ai_delay = 0.20f;
}

bool game_is_human_turn(void)
{
    if (G.state != GS_PLAYING) return false;
    if (G.mode == MODE_HUMAN_HUMAN) return true;
    if (G.mode == MODE_AI_AI) return false;
    return G.side == (G.human_white ? SIDE_WHITE : SIDE_BLACK);
}

bool game_view_flipped(void)
{
    return G.mode == MODE_HUMAN_AI && !G.human_white;
}

static bool same_move_target(Move m, int from, int to, char promo)
{
    if (m.from != from || m.to != to) return false;
    if (!m.promo) return true;
    if (!promo) return m.promo == 'q';
    return (char)tolower((unsigned char)promo) == m.promo;
}

bool game_apply_human_target(int from, int to)
{
    if (from < 0 || from >= 64 || to < 0 || to >= 64) {
        snprintf(G.status, sizeof G.status, "Invalid board square");
        return false;
    }
    game_generate_legal();
    for (int i = 0; i < G.legal_count; i++) {
        Move m = G.legal[i];
        if (same_move_target(m, from, to, 0)) {
            if (m.promo) {
                /* several legal promotions share from/to: ask the player */
                G.promo_from = from;
                G.promo_to = to;
                G.promo_cursor = 0;
                G.state = GS_PROMOTING;
                sound_play(SND_SELECT, 0.6f, 1.2f);
                snprintf(G.status, sizeof G.status, "Choose the pawn's new rank");
                return true;
            }
            start_animation(m);
            return true;
        }
    }
    char a[3], b[3];
    square_name(from, a);
    square_name(to, b);
    snprintf(G.status, sizeof G.status, "%s cannot move %s-%s",
             piece_name(piece_type(G.board[from])), a, b);
    return false;
}

static void confirm_promotion(void)
{
    static const char promo_chars[4] = { 'q', 'r', 'b', 'n' };
    char promo = promo_chars[G.promo_cursor & 3];
    for (int i = 0; i < G.legal_count; i++) {
        Move m = G.legal[i];
        if (m.from == G.promo_from && m.to == G.promo_to && m.promo == promo) {
            start_animation(m);
            return;
        }
    }
    G.state = GS_PLAYING;   /* should not happen; fail open */
}

bool game_apply_uci_move(const char *uci)
{
    if (!uci) return false;
    size_t len = strlen(uci);
    if ((len != 4 && len != 5) ||
        uci[0] < 'a' || uci[0] > 'h' || uci[1] < '1' || uci[1] > '8' ||
        uci[2] < 'a' || uci[2] > 'h' || uci[3] < '1' || uci[3] > '8')
        return false;
    if (len == 5 && !strchr("qrbnQRBN", uci[4]))
        return false;
    int from = make_sq(uci[0] - 'a', uci[1] - '1');
    int to = make_sq(uci[2] - 'a', uci[3] - '1');
    char promo = len == 5 ? uci[4] : 0;
    game_generate_legal();
    for (int i = 0; i < G.legal_count; i++) {
        Move m = G.legal[i];
        /* UCI requires a promotion suffix exactly when the move promotes. */
        if ((m.promo != 0) != (promo != 0)) continue;
        if (same_move_target(m, from, to, promo)) {
            start_animation(m);
            return true;
        }
    }
    return false;
}

static const char *initial_rank(int rank)
{
    static const char *ranks[8] = {
        "RNBQKBNR", "PPPPPPPP", "........", "........",
        "........", "........", "pppppppp", "rnbqkbnr"
    };
    return ranks[rank];
}

void game_start(int mode)
{
    engine_cancel_all();
    memset(G.board, '.', sizeof G.board);
    for (int r = 0; r < 8; r++) {
        const char *src = initial_rank(r);
        for (int f = 0; f < 8; f++)
            G.board[make_sq(f, r)] = src[f];
    }
    G.mode = mode;
    G.side = SIDE_WHITE;
    G.cursor = G.human_white || mode != MODE_HUMAN_AI
        ? make_sq(4, 1) : make_sq(4, 6);
    G.selected = -1;
    G.castle_wk = G.castle_wq = G.castle_bk = G.castle_bq = true;
    G.ep_square = -1;
    G.halfmove_clock = 0;
    G.fullmove_number = 1;
    G.uci_history[0] = '\0';
    G.last_move[0] = '\0';
    G.result[0] = '\0';
    G.result_kind = RESULT_NONE;
    G.winner_side = -1;
    G.loser_king_sq = -1;
    G.over_t = 0;
    G.check_flash = 0;
    G.check_sq = -1;
    G.resign_arm = 0;
    G.leave_arm = 0;
    G.ai_thinking = false;
    for (int i = 0; i < 2; i++)
        if (!G.engines[i].ok)
            G.engine_attempted[i] = false;
    G.ply_count = 0;
    G.ai_delay = 0.35f;
    G.ai_movetime_ms = diff_movetime[G.difficulty % DIFF_COUNT];
    G.anim.active = false;
    memset(G.captured, 0, sizeof G.captured);
    G.captured_count[0] = G.captured_count[1] = 0;
    pos_hash_count = 0;
    push_position_hash(false);
    game_generate_legal();
    set_turn_status();
    G.state = GS_PLAYING;
    sound_play(SND_START_TRUMPET, 0.85f, 1.0f);
    if (G.music_on)
        sound_music_play(MUS_THINKING, 0.42f, true);
}

void game_reset_to_title(void)
{
    engine_cancel_all();
    G.state = GS_TITLE;
    G.title_t = 0;
    G.selected = -1;
    G.anim.active = false;
    G.ai_thinking = false;
    G.result[0] = '\0';
    G.result_kind = RESULT_NONE;
    G.resign_arm = G.leave_arm = 0;
    G.check_flash = 0;
    G.check_sq = -1;
    snprintf(G.status, sizeof G.status, "Choose a mode");
    if (G.music_on)
        sound_music_play(MUS_BATTLE, 0.38f, true);
}

void game_init(int w, int h, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    for (int i = 0; i < 2; i++) {
        G.engines[i].in_fd = -1;
        G.engines[i].out_fd = -1;
    }
    G.W = w;
    G.H = h;
    G.cursor = make_sq(4, 1);
    G.selected = -1;
    G.mode = MODE_HUMAN_AI;
    G.title_row = 0;
    G.human_white = true;
    G.fast_anim = false;
    G.music_on = true;
    G.difficulty = 1;
    G.board_theme = 0;
    G.ai_movetime_ms = 450;
    G.winner_side = -1;
    G.loser_king_sq = -1;
    G.check_sq = -1;
    rng_state = seed ? seed : 0x5eed1234u;
    game_reset_to_title();
    G.state = GS_INTRO;
    G.intro_t = 0;
}

void game_shutdown(void)
{
    engine_stop(&G.engines[SIDE_WHITE]);
    engine_stop(&G.engines[SIDE_BLACK]);
}

static const int piece_value[PT_COUNT] = { 100, 320, 330, 500, 900, 0 };

static int fallback_move_priority(Move m)
{
    int score = 0;
    char victim = m.en_passant
        ? G.board[make_sq(sq_file(m.to), sq_rank(m.from))]
        : G.board[m.to];
    int victim_type = piece_type(victim);
    int attacker_type = piece_type(G.board[m.from]);
    if (victim_type >= 0)
        score += piece_value[victim_type] * 10 -
                 (attacker_type >= 0 ? piece_value[attacker_type] : 0);
    if (m.promo) score += piece_value[piece_type(m.promo)] * 10;
    if (m.castle) score += 120;
    return score;
}

static void fallback_order_moves(Move *moves, int count)
{
    /* Captures and promotions first make alpha-beta much more effective. */
    for (int i = 1; i < count; i++) {
        Move m = moves[i];
        int priority = fallback_move_priority(m);
        int j = i;
        while (j > 0 && fallback_move_priority(moves[j - 1]) < priority) {
            moves[j] = moves[j - 1];
            j--;
        }
        moves[j] = m;
    }
}

static int fallback_evaluate(int root_side)
{
    int score = 0;
    for (int sq = 0; sq < 64; sq++) {
        char p = G.board[sq];
        int type = piece_type(p);
        int side = piece_color(p);
        if (type < 0 || side < 0) continue;
        int f = sq_file(sq), r = sq_rank(sq);
        int center = 14 - abs(2 * f - 7) - abs(2 * r - 7);
        int bonus = 0;
        switch (type) {
        case PT_PAWN:
            bonus = (side == SIDE_WHITE ? r : 7 - r) * 7 + center;
            break;
        case PT_KNIGHT: bonus = center * 5; break;
        case PT_BISHOP: bonus = center * 3; break;
        case PT_ROOK:   bonus = center; break;
        case PT_QUEEN:  bonus = center * 2; break;
        case PT_KING:
            if ((side == SIDE_WHITE && r == 0) || (side == SIDE_BLACK && r == 7))
                bonus = (f == 2 || f == 6) ? 36 : 0;
            break;
        }
        int signed_value = piece_value[type] + bonus;
        score += side == root_side ? signed_value : -signed_value;
    }

    /* game_generate_legal() has already populated mobility for the side to
     * move at every search node.  This small term prevents aimless shuffling. */
    score += (G.side == root_side ? 1 : -1) * G.legal_count * 2;
    if (game_side_in_check(root_side)) score -= 35;
    if (game_side_in_check(1 - root_side)) score += 35;
    return score;
}

static int fallback_search(int depth, int alpha, int beta, int root_side, int ply)
{
    enum { MATE_SCORE = 100000 };
    game_generate_legal();
    if (G.legal_count == 0) {
        if (!game_side_in_check(G.side)) return 0;
        return G.side == root_side ? -MATE_SCORE + ply : MATE_SCORE - ply;
    }
    if (depth <= 0)
        return fallback_evaluate(root_side);

    Move moves[MAX_MOVES];
    int count = G.legal_count;
    memcpy(moves, G.legal, (size_t)count * sizeof *moves);
    fallback_order_moves(moves, count);
    PositionSnapshot saved;
    position_save(&saved);
    bool maximize = G.side == root_side;
    int best = maximize ? -MATE_SCORE * 2 : MATE_SCORE * 2;
    for (int i = 0; i < count; i++) {
        apply_move_state(&G, moves[i]);
        int score = fallback_search(depth - 1, alpha, beta, root_side, ply + 1);
        position_restore(&saved);
        if (maximize) {
            if (score > best) best = score;
            if (best > alpha) alpha = best;
        } else {
            if (score < best) best = score;
            if (best < beta) beta = best;
        }
        if (beta <= alpha) break;
    }
    return best;
}

static bool fallback_ai_move(char out[8])
{
    enum { INF = 200000 };
    game_generate_legal();
    if (!G.legal_count) return false;
    Move moves[MAX_MOVES];
    int count = G.legal_count;
    memcpy(moves, G.legal, (size_t)count * sizeof *moves);
    fallback_order_moves(moves, count);

    int skill = G.difficulty;
    if (skill < 0) skill = 0;
    if (skill >= DIFF_COUNT) skill = DIFF_COUNT - 1;
    int depth = skill + 1;       /* Squire 1 ply, Knight 2, Warlord 3 */
    int root_side = G.side;
    int best = 0, best_score = -INF, ties = 0;
    PositionSnapshot saved;
    position_save(&saved);
    for (int i = 0; i < count; i++) {
        apply_move_state(&G, moves[i]);
        int score = fallback_search(depth - 1, -INF, INF, root_side, 1);
        position_restore(&saved);
        /* The easy levels remain fallible; Warlord plays the search result. */
        if (skill == 0)
            score += (int)(xrng() % 181u) - 90;
        else if (skill == 1)
            score += (int)(xrng() % 17u) - 8;
        if (score > best_score) {
            best_score = score;
            best = i;
            ties = 1;
        } else if (score == best_score && (xrng() % (unsigned)(++ties)) == 0) {
            best = i;
        }
    }
    game_move_to_uci(moves[best], out);
    return true;
}

static void ai_fallback_turn(void)
{
    char move[8] = {0};
    if (fallback_ai_move(move)) {
        snprintf(G.status, sizeof G.status, "Built-in %s AI chooses %s",
                 difficulty_name(G.difficulty), move);
        game_apply_uci_move(move);
    }
}

static void ai_turn(void)
{
    int side = G.side;
    if (G.ai_thinking) {
        char move[8] = {0};
        int r = engine_poll(&G.engines[side], move, NULL, 0);
        if (r == ENGINE_PENDING) return;
        G.ai_thinking = false;
        if (r == ENGINE_DONE && game_apply_uci_move(move)) return;
        ai_fallback_turn();
        return;
    }

    if (!G.engine_attempted[side]) {
        G.engine_attempted[side] = true;
        if (engine_request(&G.engines[side], G.uci_history, G.ai_movetime_ms)) {
            G.ai_thinking = true;
            snprintf(G.status, sizeof G.status, "%s computer engine starting",
                     side_name(side));
            return;
        }
        ai_fallback_turn();
        return;
    }

    if (G.engines[side].ok && engine_request(&G.engines[side], G.uci_history,
                                              G.ai_movetime_ms)) {
        G.ai_thinking = true;
        snprintf(G.status, sizeof G.status, "%s %s thinking", side_name(side),
                 G.engines[side].name[0] ? G.engines[side].name : "engine");
        return;
    }
    ai_fallback_turn();
}

enum {
    ANIM_SND_STEP_1 = 1u << 0,
    ANIM_SND_STEP_2 = 1u << 1,
    ANIM_SND_STEP_3 = 1u << 2,
    ANIM_SND_STEP_4 = 1u << 3,
    ANIM_SND_HIT    = 1u << 4,
    ANIM_SND_FALL   = 1u << 5,
};

static void play_anim_sound_at(float threshold, uint32_t flag, int id, float vol, float pitch)
{
    if (G.anim.t < threshold || (G.anim.sound_flags & flag)) return;
    G.anim.sound_flags |= flag;
    sound_play(id, vol, pitch);
}

static void update_animation_sounds(void)
{
    if (!G.anim.active) return;
    if (G.fast_anim) {
        if (!(G.anim.sound_flags & ANIM_SND_HIT)) {
            G.anim.sound_flags |= ANIM_SND_HIT;
            sound_play(G.anim.move.capture ? SND_CAPTURE : SND_MOVE,
                       G.anim.move.capture ? 0.82f : 0.62f, 1.0f);
        }
        return;
    }

    if (G.anim.move.capture) {
        play_anim_sound_at(0.07f, ANIM_SND_STEP_1, SND_MOVE, 0.52f, 0.92f);
        play_anim_sound_at(0.18f, ANIM_SND_STEP_2, SND_MOVE, 0.48f, 1.04f);
        play_anim_sound_at(0.28f, ANIM_SND_STEP_3, SND_MOVE, 0.50f, 0.98f);
        /* the clash lands on the visible strike, slightly leading it to
         * cover the remaining sink latency; pitch varies per battle */
        if (G.anim.t >= 0.49f && !(G.anim.sound_flags & ANIM_SND_HIT)) {
            G.anim.sound_flags |= ANIM_SND_HIT;
            sound_play(SND_CAPTURE, 0.92f, 0.94f + (xrng() & 7) * 0.02f);
        }
        play_anim_sound_at(0.78f, ANIM_SND_FALL, SND_FALL, 0.72f, 1.0f);
    } else {
        play_anim_sound_at(0.08f, ANIM_SND_STEP_1, SND_MOVE, 0.52f, 0.92f);
        play_anim_sound_at(0.30f, ANIM_SND_STEP_2, SND_MOVE, 0.48f, 1.02f);
        play_anim_sound_at(0.54f, ANIM_SND_STEP_3, SND_MOVE, 0.50f, 0.96f);
        play_anim_sound_at(0.78f, ANIM_SND_STEP_4, SND_MOVE, 0.44f, 1.06f);
    }
}

void game_tick(void)
{
    G.frame_count++;
    if (G.check_flash > 0)
        G.check_flash -= TICK_DT;
    if (G.resign_arm > 0) {
        G.resign_arm -= TICK_DT;
        if (G.resign_arm <= 0 && G.state == GS_PLAYING &&
            !strncmp(G.status, "Press R again", 13))
            set_turn_status();
    }
    if (G.leave_arm > 0) {
        G.leave_arm -= TICK_DT;
        if (G.leave_arm <= 0 && G.state == GS_PLAYING &&
            !strncmp(G.status, "Press N again", 13))
            set_turn_status();
    }

    if (G.state == GS_INTRO) {
        G.intro_t += TICK_DT;
        if (G.intro_t > 5.2f)
            game_reset_to_title();
        return;
    }
    if (G.state == GS_TITLE) {
        G.title_t += TICK_DT;
        return;
    }
    if (G.state == GS_GAMEOVER) {
        G.over_t += TICK_DT;
        return;
    }
    if (G.state == GS_ANIMATING && G.anim.active) {
        G.anim.t += TICK_DT / G.anim.duration;
        update_animation_sounds();
        if (G.anim.t >= 1.0f)
            finish_animation();
        return;
    }
    if (G.state == GS_PLAYING && !game_is_human_turn() &&
        G.resign_arm <= 0 && G.leave_arm <= 0) {
        G.ai_delay -= TICK_DT;
        if (G.ai_delay <= 0)
            ai_turn();
    }
}

static int legal_target_count(int from)
{
    int count = 0;
    bool seen[64] = { false };
    for (int i = 0; i < G.legal_count; i++) {
        if (G.legal[i].from != from || seen[G.legal[i].to]) continue;
        seen[G.legal[i].to] = true;
        count++;
    }
    return count;
}

static void set_cursor_status(void)
{
    char sq[3];
    square_name(G.cursor, sq);
    if (G.selected >= 0) {
        char selected_sq[3];
        square_name(G.selected, selected_sq);
        snprintf(G.status, sizeof G.status, "%s %s selected - choose a highlighted square",
                 piece_name(piece_type(G.board[G.selected])), selected_sq);
        return;
    }
    char p = G.board[G.cursor];
    int color = piece_color(p);
    if (color == G.side) {
        snprintf(G.status, sizeof G.status, "%s %s - ENTER to select",
                 sq, piece_name(piece_type(p)));
    } else if (color >= 0) {
        snprintf(G.status, sizeof G.status, "%s %s %s - choose a %s piece",
                 sq, side_name(color), piece_name(piece_type(p)), side_name(G.side));
    } else {
        snprintf(G.status, sizeof G.status, "%s empty - choose a %s piece",
                 sq, side_name(G.side));
    }
}

static void select_square(int sq)
{
    G.selected = sq;
    G.resign_arm = G.leave_arm = 0;
    sound_play(SND_SELECT, 0.55f, 1.10f);
    char name[3];
    square_name(sq, name);
    int targets = legal_target_count(sq);
    snprintf(G.status, sizeof G.status, "%s %s selected - %d legal move%s",
             piece_name(piece_type(G.board[sq])), name, targets,
             targets == 1 ? "" : "s");
}

static void move_cursor(int df, int dr)
{
    if (game_view_flipped()) {
        df = -df;
        dr = -dr;
    }
    int old = G.cursor;
    int f = sq_file(G.cursor) + df;
    int r = sq_rank(G.cursor) + dr;
    if (f < 0) f = 0;
    if (f > 7) f = 7;
    if (r < 0) r = 0;
    if (r > 7) r = 7;
    G.cursor = make_sq(f, r);
    if (G.cursor != old) {
        G.resign_arm = G.leave_arm = 0;
        sound_play(SND_SELECT, 0.24f, 0.82f + 0.05f * sq_file(G.cursor));
        set_cursor_status();
    }
}

static void toggle_music(void)
{
    G.music_on = !G.music_on;
    if (!G.music_on) {
        sound_music_stop(0.4f);
        return;
    }
    if (G.state == GS_TITLE || G.state == GS_INTRO)
        sound_music_play(MUS_BATTLE, 0.38f, true);
    else if (G.state != GS_GAMEOVER)
        sound_music_play(MUS_THINKING, 0.42f, true);
}

static bool title_row_enabled(int row)
{
    if (row == MENU_SIDE) return G.mode == MODE_HUMAN_AI;
    if (row == MENU_DIFF) return G.mode != MODE_HUMAN_HUMAN;
    return true;
}

static void title_move_row(int dir)
{
    do {
        G.title_row = (G.title_row + dir + MENU_COUNT) % MENU_COUNT;
    } while (!title_row_enabled(G.title_row));
    sound_play(SND_SELECT, 0.35f, 0.95f);
}

static void title_adjust(int dir)
{
    switch (G.title_row) {
    case MENU_MODE:
        G.mode = (G.mode + dir + MODE_COUNT) % MODE_COUNT;
        break;
    case MENU_SIDE:
        G.human_white = !G.human_white;
        break;
    case MENU_DIFF:
        G.difficulty = (G.difficulty + dir + DIFF_COUNT) % DIFF_COUNT;
        break;
    case MENU_BOARD:
        G.board_theme = (G.board_theme + dir + BOARD_THEME_COUNT) % BOARD_THEME_COUNT;
        render_set_theme(G.board_theme);
        break;
    case MENU_MUSIC:
        toggle_music();
        break;
    case MENU_SPEED:
        G.fast_anim = !G.fast_anim;
        break;
    default:
        break;
    }
    sound_play(SND_SELECT, 0.45f, 1.08f);
}

static void resign_key(void)
{
    int resigner;
    if (G.mode == MODE_AI_AI) return;
    if (G.mode == MODE_HUMAN_HUMAN) {
        resigner = G.side;   /* whoever is to move throws in the towel */
    } else {
        /* the human may resign even while the engine is thinking */
        resigner = G.human_white ? SIDE_WHITE : SIDE_BLACK;
    }
    if (G.resign_arm > 0) {
        game_over(RESULT_RESIGN, 1 - resigner);
        return;
    }
    G.leave_arm = 0;
    G.resign_arm = 2.5f;
    snprintf(G.status, sizeof G.status, "Press R again to resign");
}

static void leave_key(void)
{
    if (G.leave_arm > 0) {
        game_reset_to_title();
        return;
    }
    G.resign_arm = 0;
    G.leave_arm = 2.5f;
    snprintf(G.status, sizeof G.status, "Press N again to leave this game");
}

void game_handle_key(int key)
{
    if (key == 'q' || key == 'Q') {
        G.quit = true;
        return;
    }
    if (key != 'r' && key != 'R' && key != 'n' && key != 'N' &&
        (G.resign_arm > 0 || G.leave_arm > 0)) {
        G.resign_arm = G.leave_arm = 0;
        if (G.state == GS_PLAYING)
            set_turn_status();
    }
    if (key == 'm' || key == 'M') {
        toggle_music();
        return;
    }
    if (G.state == GS_INTRO) {
        game_reset_to_title();   /* any key skips the intro */
        return;
    }
    if (G.state == GS_TITLE) {
        if (key == KEY_UP || key == 'w' || key == 'W')
            title_move_row(-1);
        else if (key == KEY_DOWN || key == 's' || key == 'S')
            title_move_row(1);
        else if (key == KEY_LEFT || key == 'a' || key == 'A')
            title_adjust(-1);
        else if (key == KEY_RIGHT || key == 'd' || key == 'D')
            title_adjust(1);
        else if (key == 'f' || key == 'F')
            G.fast_anim = !G.fast_anim;
        else if (key == KEY_ENTER || key == ' ') {
            if (G.title_row == MENU_START)
                game_start(G.mode);
            else
                title_adjust(1);
        }
        return;
    }
    if (G.state == GS_GAMEOVER) {
        /* absorb the key mash that ended the game (e.g. a held R resign
         * chaining straight into a rematch) until the banner has landed */
        if (G.over_t < 0.6f) return;
        if (key == KEY_ENTER || key == ' ' || key == 'n' || key == 'N')
            game_reset_to_title();
        else if (key == 'r' || key == 'R')
            game_start(G.mode);   /* rematch */
        return;
    }
    if (G.state == GS_ANIMATING) {
        if (key == 's' || key == 'S' || key == KEY_ENTER || key == ' ')
            G.anim.t = 1.0f;
        return;
    }
    if (G.state == GS_PROMOTING) {
        if (key == KEY_LEFT || key == 'a' || key == 'A' ||
            key == KEY_UP || key == 'w' || key == 'W') {
            G.promo_cursor = (G.promo_cursor + 3) % 4;
            sound_play(SND_SELECT, 0.4f, 1.0f);
        } else if (key == KEY_RIGHT || key == 'd' || key == 'D' ||
                   key == KEY_DOWN || key == 's' || key == 'S') {
            G.promo_cursor = (G.promo_cursor + 1) % 4;
            sound_play(SND_SELECT, 0.4f, 1.0f);
        } else if (key == KEY_ENTER || key == ' ') {
            confirm_promotion();
        } else if (key == KEY_ESC) {
            G.state = GS_PLAYING;
            G.selected = -1;
            set_turn_status();
        }
        return;
    }
    if (key == KEY_ESC) {
        G.selected = -1;
        G.resign_arm = G.leave_arm = 0;
        set_turn_status();
        return;
    }
    if (key == 'f' || key == 'F') {
        G.fast_anim = !G.fast_anim;
        return;
    }
    if (key == 'n' || key == 'N') {
        leave_key();
        return;
    }
    if (key == 'r' || key == 'R') {
        resign_key();
        return;
    }
    if (!game_is_human_turn()) return;

    if (key == KEY_LEFT || key == 'a' || key == 'A') move_cursor(-1, 0);
    else if (key == KEY_RIGHT || key == 'd' || key == 'D') move_cursor(1, 0);
    else if (key == KEY_UP || key == 'w' || key == 'W') move_cursor(0, 1);
    else if (key == KEY_DOWN || key == 's' || key == 'S' ||
             key == 'x' || key == 'X') move_cursor(0, -1);
    else if (key == KEY_ENTER || key == ' ') {
        if (G.selected < 0) {
            if (piece_color(G.board[G.cursor]) == G.side) {
                select_square(G.cursor);
            } else {
                set_cursor_status();
                sound_play(SND_SELECT, 0.30f, 0.68f);
            }
        } else {
            if (G.cursor == G.selected) {
                G.selected = -1;
                G.resign_arm = G.leave_arm = 0;
                sound_play(SND_SELECT, 0.42f, 0.70f);
                set_turn_status();
            } else if (!game_apply_human_target(G.selected, G.cursor)) {
                if (piece_color(G.board[G.cursor]) == G.side) {
                    select_square(G.cursor);
                }
            }
        }
    }
}
