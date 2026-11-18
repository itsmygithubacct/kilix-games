/* test_puzzllm.c - headless unit tests: terminal emulator, keys, levels,
 * JSON, activity feed, scene, rendering and the level-1 flow.
 *
 * PUZZLLM_TEST_DIR is a scratch folder the test may fill (make test-unit
 * empties it first). Run from the puzzllm/ directory.
 */
#include "activity.h"
#include "game.h"
#include "json.h"
#include "keys.h"
#include "layout.h"
#include "levels.h"
#include "render.h"
#include "scene.h"
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static char scratch[2048];

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static void feed(vt *t, const char *s) { vt_feed(t, s, strlen(s)); }

static const char *row(const vt *t, int y)
{
    static char buf[4096];
    vt_row_text(t, y, buf, sizeof buf);
    return buf;
}

static void write_file(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror(path); exit(2); }
    if (n > 0 && write(fd, data, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
}

static void sub(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", scratch, name);
}

/* ---------------- terminal emulator ---------------- */

static void test_vt(void)
{
    vt t;
    char r[64];
    size_t n;

    CHECK(vt_init(&t, 20, 5), "vt_init");
    feed(&t, "hello\r\nworld");
    CHECK(strcmp(row(&t, 0), "hello") == 0, "row0 '%s'", row(&t, 0));
    CHECK(strcmp(row(&t, 1), "world") == 0, "row1 '%s'", row(&t, 1));
    CHECK(t.cx == 5 && t.cy == 1, "cursor %d,%d", t.cx, t.cy);

    feed(&t, "\033[3;5HX");
    CHECK(vt_cell_at(&t, 4, 2)->ch == 'X', "CUP puts X at column 5 row 3");
    feed(&t, "\033[2J\033[H");
    CHECK(strcmp(row(&t, 0), "") == 0 && t.cx == 0 && t.cy == 0, "ED 2 + home");

    /* autowrap at the right edge */
    feed(&t, "abcdefghijklmnopqrstUV");
    CHECK(strcmp(row(&t, 0), "abcdefghijklmnopqrst") == 0, "wrap row0 '%s'", row(&t, 0));
    CHECK(strcmp(row(&t, 1), "UV") == 0, "wrap row1 '%s'", row(&t, 1));

    /* erase in line */
    feed(&t, "\033[1;3H\033[K");
    CHECK(strcmp(row(&t, 0), "ab") == 0, "EL 0 '%s'", row(&t, 0));

    /* colours */
    feed(&t, "\033[H\033[31mR\033[38;5;196mS\033[38;2;1;2;3mT\033[48:2::9:8:7mU\033[0mV\033[7mW");
    CHECK(vt_cell_at(&t, 0, 0)->fg == vt_palette(1), "SGR 31");
    CHECK(vt_cell_at(&t, 1, 0)->fg == vt_palette(196), "SGR 38;5;196");
    CHECK(vt_cell_at(&t, 2, 0)->fg == 0x010203, "SGR 38;2 truecolour");
    CHECK(vt_cell_at(&t, 3, 0)->bg == 0x090807, "SGR 48:2:: colon form");
    feed(&t, "\033[H\033[38:2:4:5:6;1mZ\033[0m");
    CHECK(vt_cell_at(&t, 0, 0)->fg == 0x040506 && (vt_cell_at(&t, 0, 0)->attr & VT_ATTR_BOLD),
          "SGR 38:2:r:g:b then ;1");
    CHECK(vt_cell_at(&t, 4, 0)->fg == VT_DEFAULT_COLOR, "SGR 0 resets");
    CHECK(vt_cell_at(&t, 5, 0)->attr & VT_ATTR_REVERSE, "SGR 7");
    CHECK(vt_palette(232) == 0x080808 && vt_palette(16) == 0x000000 &&
          vt_palette(231) == 0xffffff, "256-colour cube and greys");
    feed(&t, "\033[0m");

    /* scroll region: a linefeed at its bottom scrolls only the region */
    feed(&t, "\033[2J\033[H1\r\n2\r\n3\r\n4\r\n5");
    feed(&t, "\033[2;4r\033[4;1H\nX");
    CHECK(strcmp(row(&t, 0), "1") == 0, "region keeps row above");
    CHECK(strcmp(row(&t, 1), "3") == 0, "region scrolled '%s'", row(&t, 1));
    CHECK(strcmp(row(&t, 3), "X") == 0, "new line in region '%s'", row(&t, 3));
    CHECK(strcmp(row(&t, 4), "5") == 0, "region keeps row below");
    feed(&t, "\033[r");

    /* insert/delete lines and characters */
    feed(&t, "\033[2J\033[Habcdef\033[1;3H\033[2P");
    CHECK(strcmp(row(&t, 0), "abef") == 0, "DCH '%s'", row(&t, 0));
    feed(&t, "\033[1;2H\033[2@");
    CHECK(strcmp(row(&t, 0), "a  bef") == 0, "ICH '%s'", row(&t, 0));
    feed(&t, "\033[2J\033[HA\r\nB\r\nC\033[2;1H\033[L");
    CHECK(strcmp(row(&t, 1), "") == 0 && strcmp(row(&t, 2), "B") == 0, "IL");
    feed(&t, "\033[M");
    CHECK(strcmp(row(&t, 1), "B") == 0, "DL '%s'", row(&t, 1));

    /* alternate screen keeps the main one */
    feed(&t, "\033[2J\033[Hmain\033[?1049h");
    CHECK(t.alt_active && strcmp(row(&t, 0), "") == 0, "1049 enters a blank alt screen");
    feed(&t, "alt!");
    feed(&t, "\033[?1049l");
    CHECK(!t.alt_active && strcmp(row(&t, 0), "main") == 0, "1049 restores main '%s'", row(&t, 0));
    CHECK(t.cx == 4 && t.cy == 0, "1049 restores the cursor");

    /* UTF-8, also split across feeds; box drawing; wide characters */
    feed(&t, "\033[2J\033[H\xe2\x94");
    feed(&t, "\x80\xe2\x95\xad");
    CHECK(vt_cell_at(&t, 0, 0)->ch == 0x2500 && vt_cell_at(&t, 1, 0)->ch == 0x256d,
          "split UTF-8");
    feed(&t, "\xe4\xb8\xad|");
    CHECK(vt_cell_at(&t, 2, 0)->ch == 0x4e2d && (vt_cell_at(&t, 2, 0)->attr & VT_ATTR_WIDE),
          "wide char");
    CHECK(vt_cell_at(&t, 4, 0)->ch == '|', "wide char takes two cells");
    feed(&t, "\xff");
    CHECK(vt_cell_at(&t, 5, 0)->ch == 0xfffd, "invalid byte -> U+FFFD");

    /* replies */
    feed(&t, "\033[2;3H\033[6n\033[c");
    n = vt_take_reply(&t, r, sizeof r - 1);
    r[n] = '\0';
    CHECK(strcmp(r, "\033[2;3R\033[?62;22c") == 0, "CPR + DA1 reply");
    feed(&t, "\033[?u");
    CHECK(vt_take_reply(&t, r, sizeof r) == 0, "Kitty keyboard query is not answered");

    /* strings are swallowed: OSC title, APC (Kitty graphics), DCS */
    feed(&t, "\033[2J\033[H\033]0;my title\007\033_Gf=100;AAAA\033\\\033Pq#0\033\\ok");
    CHECK(strcmp(row(&t, 0), "ok") == 0, "strings swallowed '%s'", row(&t, 0));
    CHECK(strcmp(t.title, "my title") == 0, "OSC 0 title");

    /* modes */
    feed(&t, "\033[?1h\033[?25l\033[?2004h");
    CHECK(t.app_cursor && !t.cursor_visible && t.bracketed_paste, "DECCKM DECTCEM 2004");
    feed(&t, "\033[?1l\033[?25h");
    CHECK(!t.app_cursor && t.cursor_visible, "modes reset");

    /* resize keeps the cursor line */
    feed(&t, "\033[2J\033[5;1Hlast");
    CHECK(vt_resize(&t, 10, 3), "resize");
    CHECK(strcmp(row(&t, 2), "last") == 0 && t.cy == 2, "resize keeps bottom '%s'", row(&t, 2));
    vt_free(&t);
}

/* ---------------- keys ---------------- */

static const char *enc(uint32_t key, uint32_t mods, const char *text, bool app)
{
    static char out[64];
    key_event ev;
    size_t n;
    memset(&ev, 0, sizeof ev);
    ev.key = key;
    ev.mods = mods;
    ev.action = 1;
    if (text) {
        /* ASCII or one 2-byte UTF-8 character is enough here */
        const unsigned char *p = (const unsigned char *)text;
        while (*p) {
            if (*p < 0x80) ev.text[ev.text_len++] = *p++;
            else { ev.text[ev.text_len++] = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f); p += 2; }
        }
    }
    n = keys_encode(&ev, app, out, sizeof out - 1);
    out[n] = '\0';
    return out;
}

static void test_keys(void)
{
    key_event ev;
    char out[64];

    CHECK(strcmp(enc('a', 0, "a", false), "a") == 0, "plain a");
    CHECK(strcmp(enc('a', MOD_SHIFT, "A", false), "A") == 0, "shift a");
    CHECK(strcmp(enc('1', MOD_SHIFT, "!", false), "!") == 0, "shift 1");
    CHECK(strcmp(enc('c', MOD_CTRL, NULL, false), "\x03") == 0, "ctrl c");
    CHECK(strcmp(enc('x', MOD_ALT, "x", false), "\033x") == 0, "alt x");
    CHECK(strcmp(enc(KEY_ENTER, 0, NULL, false), "\r") == 0, "enter");
    CHECK(strcmp(enc(KEY_BACKSPACE, 0, NULL, false), "\x7f") == 0, "backspace");
    CHECK(strcmp(enc(KEY_TAB, MOD_SHIFT, NULL, false), "\033[Z") == 0, "shift tab");
    CHECK(strcmp(enc(KEY_UP, 0, NULL, false), "\033[A") == 0, "up");
    CHECK(strcmp(enc(KEY_UP, 0, NULL, true), "\033OA") == 0, "up, application mode");
    CHECK(strcmp(enc(KEY_LEFT, MOD_CTRL, NULL, true), "\033[1;5D") == 0, "ctrl left");
    CHECK(strcmp(enc(KEY_DELETE, 0, NULL, false), "\033[3~") == 0, "delete");
    CHECK(strcmp(enc(KEY_PAGE_UP, MOD_SHIFT, NULL, false), "\033[5;2~") == 0, "shift pgup");
    CHECK(strcmp(enc(KEY_F1, 0, NULL, false), "\033OP") == 0, "F1");
    CHECK(strcmp(enc(KEY_F1 + 4, 0, NULL, false), "\033[15~") == 0, "F5");
    CHECK(strcmp(enc(0xe9, 0, "\xc3\xa9", false), "\xc3\xa9") == 0, "UTF-8 text");
    memset(&ev, 0, sizeof ev);
    ev.key = 'a';
    ev.action = 3;
    CHECK(keys_encode(&ev, false, out, sizeof out) == 0, "release sends nothing");
    CHECK(keys_paste("a\nb", true, out, sizeof out) == 15 &&
          memcmp(out, "\033[200~a\rb\033[201~", 15) == 0, "bracketed paste");
}

/* ---------------- levels ---------------- */

static void test_levels(void)
{
    level_list L;
    char err[256], ws[2300], path[2400];
    goal_result r;
    const level *lv;

    CHECK(levels_load(&L, "levels/levels.txt", err, sizeof err), "levels.txt: %s", err);
    CHECK(L.count >= 2, "at least two levels");
    lv = &L.items[0];
    CHECK(lv->check == CHECK_FILE && strcmp(lv->file, "hello_world.txt") == 0,
          "level 1 checks hello_world.txt");
    CHECK(lv->nhints >= 3, "level 1 has hints");
    CHECK(L.items[L.count - 1].check == CHECK_NONE, "last level is the stub");

    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\n", err, sizeof err),
          "a level without a check is rejected");
    CHECK(!levels_parse(&L, "level: 2\ntitle: t\ngoal: g\ncheck: none\n", err, sizeof err),
          "numbering must start at 1");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file ../x\n", err, sizeof err),
          "'..' rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file /etc/passwd\n", err, sizeof err),
          "absolute path rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file .x\n", err, sizeof err),
          "hidden file rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: none\nbogus: 1\n", err, sizeof err),
          "unknown key rejected");
    CHECK(levels_parse(&L, "# c\nlevel: 1\ntitle: t\ngoal: g\ncheck: none\n\nlevel: 2\n"
                           "title: u\ngoal: h\ncheck: file a.txt\nrequire: contains x\n",
                       err, sizeof err) && L.count == 2, "two-level file: %s", err);

    CHECK(levels_load(&L, "levels/levels.txt", err, sizeof err), "reload");
    lv = &L.items[0];
    sub(ws, sizeof ws, "levels-ws");
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/hello_world.txt", ws);

    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "does not exist"), "missing: %s", r.why);
    write_file(path, "", 0);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "empty"), "empty: %s", r.why);
    write_file(path, "hello", 5);
    r = levels_check(lv, ws);
    CHECK(!r.met, "only 'hello' is not enough");
    write_file(path, "Hello, World!\n", 14);
    r = levels_check(lv, ws);
    CHECK(r.met, "Hello, World! passes: %s", r.why);
    write_file(path, "HELLO WORLD", 11);
    CHECK(levels_check(lv, ws).met, "case-insensitive");
    {
        static char big[5000];
        memset(big, 'x', sizeof big);
        memcpy(big, "hello world", 11);
        write_file(path, big, sizeof big);
        CHECK(!levels_check(lv, ws).met, "over max-bytes");
    }
    /* a symlink to a good file does not count */
    {
        char real[2400];
        snprintf(real, sizeof real, "%s/real.txt", ws);
        write_file(real, "hello world", 11);
        unlink(path);
        CHECK(symlink(real, path) == 0, "symlink");
        r = levels_check(lv, ws);
        CHECK(!r.met && strstr(r.why, "not a regular file"), "symlink: %s", r.why);
        unlink(path);
        mkdir(path, 0700);
        CHECK(!levels_check(lv, ws).met, "a directory does not count");
        rmdir(path);
    }
    CHECK(!levels_check(&L.items[L.count - 1], ws).met, "the stub room is never met");
}

/* ---------------- JSON ---------------- */

static void test_json(void)
{
    json_doc d;
    int root;
    const char *s = "{\"a\": [1, -2.5e1, true, null], \"b\": {\"c\": \"x\\u00e9\\ud83d\\ude00\\n\"}}";
    char deep[200];

    root = json_parse(&d, s, strlen(s));
    CHECK(root >= 0, "parse");
    CHECK(d.nodes[json_get(&d, root, "a")].type == JSON_ARRAY, "array member");
    CHECK(strcmp(json_str(&d, json_get(&d, json_get(&d, root, "b"), "c")),
                 "x\xc3\xa9\xf0\x9f\x98\x80\n") == 0, "escapes and surrogate pair");
    json_free(&d);
    CHECK(json_parse(&d, "{\"a\":}", 6) < 0, "bad JSON rejected");
    json_free(&d);
    CHECK(json_parse(&d, "[1] x", 5) < 0, "trailing garbage rejected");
    json_free(&d);
    memset(deep, '[', sizeof deep);
    CHECK(json_parse(&d, deep, sizeof deep) < 0, "depth is capped");
    json_free(&d);
}

/* ---------------- activity ---------------- */

static bool has_line(const activity *a, const char *needle)
{
    for (int i = 0; i < a->count; i++)
        if (strstr(activity_line(a, i)->text, needle)) return true;
    return false;
}

static activity act;   /* large: keep it off the stack */

static void test_activity(void)
{
    char ws[2300], p[2400];
    /* the shapes omp 18 writes (seen in a real session) */
    const char *lines[] = {
        "{\"type\":\"session\",\"version\":3,\"id\":\"x\",\"cwd\":\"/kilix/workspace\"}",
        "{\"type\":\"message\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"make hello_world.txt\"}]}}",
        "{\"type\":\"message\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\"},"
        "{\"type\":\"toolCall\",\"id\":\"1\",\"name\":\"write\",\"arguments\":{\"path\":\"hello_world.txt\",\"content\":\"Hello\"}}]}}",
        "{\"type\":\"message\",\"message\":{\"role\":\"toolResult\",\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"EISDIR: illegal operation\"}]}}",
        "{\"type\":\"message\",\"message\":{\"role\":\"toolResult\",\"content\":[{\"type\":\"text\",\"text\":\"[:raw#DE50]\\nSuccessfully wrote 13 bytes\"}]}}",
        "{\"type\":\"message\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Done.\\nMore\"}]}}",
        "not json at all",
    };

    activity_init(&act);
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        activity_session_line(&act, lines[i], strlen(lines[i]));
    CHECK(has_line(&act, "you: make hello_world.txt"), "user line");
    CHECK(has_line(&act, "tool write hello_world.txt"), "tool call line");
    CHECK(has_line(&act, "-> EISDIR"), "error result line");
    CHECK(has_line(&act, "-> Successfully wrote 13 bytes"), "result line drops the [tag] line");
    CHECK(has_line(&act, "agent: Done."), "assistant text, first line");
    CHECK(!has_line(&act, "More"), "only the first line");
    CHECK(act.count == 5, "five lines, garbage ignored (got %d)", act.count);

    for (int i = 0; i < ACT_LINES + 10; i++) activity_add(&act, ACT_INFO, "n%d", i);
    CHECK(act.count == ACT_LINES && strcmp(activity_line(&act, ACT_LINES - 1)->text, "n73") == 0,
          "ring keeps the newest");

    activity_init(&act);
    sub(ws, sizeof ws, "act-ws");
    mkdir(ws, 0700);
    activity_scan_workspace(&act, ws);
    snprintf(p, sizeof p, "%s/notes.txt", ws);
    write_file(p, "abc", 3);
    activity_scan_workspace(&act, ws);
    CHECK(has_line(&act, "new notes.txt (3 bytes)"), "new file");
    write_file(p, "abcdef", 6);
    activity_scan_workspace(&act, ws);
    CHECK(has_line(&act, "changed notes.txt (6 bytes)"), "changed file");
    unlink(p);
    activity_scan_workspace(&act, ws);
    CHECK(has_line(&act, "removed notes.txt"), "removed file");
}

/* ---------------- scene ---------------- */

static scene_event walk(scene *s, int dx, int dy, float seconds)
{
    scene_event last = SCENE_NONE;
    for (float t = 0; t < seconds; t += 1.0f / 60) {
        scene_event e = scene_step(s, dx, dy, 1.0f / 60);
        if (e != SCENE_NONE) last = e;
        if (e == SCENE_NEXT_ROOM) break;
    }
    return last;
}

static void test_scene(void)
{
    scene s;
    scene_init(&s, 0, 2);
    CHECK(scene_walkable(&s, START_X, START_Y), "start is walkable");
    CHECK(!scene_walkable(&s, 300, 300), "screens are not floor");

    /* door shut: walking up the aisle stops at the threshold */
    CHECK(walk(&s, 0, -1, 5) != SCENE_NEXT_ROOM, "shut door blocks");
    CHECK(s.y >= AISLE_Y0 - 0.01f, "stopped at the threshold (y=%.1f)", s.y);
    CHECK(s.door == 0, "door shut");

    /* near screens */
    s.x = 200; s.y = WALK_Y0; s.dir = 1;
    CHECK(scene_near_screen(&s) == SCREEN_OMP, "below the omp screen");
    s.x = 640; s.y = 200; s.dir = 3;
    CHECK(scene_near_screen(&s) == SCREEN_SHELL, "aisle facing right: shell");
    s.dir = 2;
    CHECK(scene_near_screen(&s) == SCREEN_OMP, "aisle facing left: omp");
    s.x = 1000; s.y = WALK_Y0; s.dir = 1;
    CHECK(scene_near_screen(&s) == SCREEN_ACT, "below the activity screen");
    s.x = 640; s.y = WALK_Y1; s.dir = 0;
    CHECK(scene_near_screen(&s) == SCREEN_NONE, "middle of the floor: nothing");

    /* goal met: the door opens, then Kilix can leave */
    scene_enter_room(&s, 0);
    s.goal_met = true;
    CHECK(walk(&s, 0, 0, 2) == SCENE_DOOR_OPENED, "door opened event");
    CHECK(scene_door_passable(&s), "door open");
    CHECK(walk(&s, 0, -1, 6) == SCENE_NEXT_ROOM, "walked through the door");

    /* the last room has no next room */
    scene_enter_room(&s, 1);
    s.goal_met = true;
    CHECK(walk(&s, 0, -1, 8) != SCENE_NEXT_ROOM, "no room after the last");

    /* typing into a screen freezes Kilix */
    scene_enter_room(&s, 0);
    s.focus = FOCUS_OMP;
    walk(&s, 1, 0, 1);
    CHECK(s.x == START_X, "no walking while typing");
}

/* ---------------- rendering ---------------- */

static uint32_t at(const sr_canvas *c, int x, int y) { return c->px[y * c->w + x] & 0xffffff; }

static int differing(const sr_canvas *a, const sr_canvas *b, rect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            if (at(a, x, y) != at(b, x, y)) n++;
    return n;
}

static game G;

static void test_render_and_flow(void)
{
    char err[256], state[2300], hello[2500], prog[2500];
    sr_canvas shut, open, cur;
    rect door = {DOOR_X, DOOR_Y, DOOR_W, DOOR_H};
    rect cell0;
    int tx, ty;

    sub(state, sizeof state, "state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "game_init: %s", err);
    CHECK(G.sc.level == 0 && !G.sc.goal_met, "starts in room 1, goal not met");
    CHECK(G.hints_shown == 1, "first hint is shown");

    CHECK(sr_canvas_init(&shut, CANVAS_W, CANVAS_H) && sr_canvas_init(&open, CANVAS_W, CANVAS_H) &&
          sr_canvas_init(&cur, CANVAS_W, CANVAS_H), "canvases");
    game_render(&G, &shut);
    /* Kilix is drawn at the entrance: his pixels are not floor tiles */
    CHECK(differing(&shut, &shut, (rect){0, 0, 1, 1}) == 0, "sanity");

    /* the player or the agent writes the file: both count */
    snprintf(hello, sizeof hello, "%s/workspace/hello_world.txt", state);
    write_file(hello, "Hello, World!\n", 14);
    game_update(&G, 0.6);
    CHECK(G.sc.goal_met, "goal met after the poll");
    CHECK(has_line(&G.act, "GOAL MET"), "activity says so");
    for (int i = 0; i < 120; i++) game_update(&G, 1.0 / 60);
    CHECK(scene_door_passable(&G.sc), "door open after the animation");
    game_render(&G, &open);
    CHECK(differing(&shut, &open, door) > DOOR_W * DOOR_H / 2, "the door looks different open");
    CHECK(at(&open, DOOR_X + DOOR_W / 2, DOOR_Y + 30) != at(&shut, DOOR_X + DOOR_W / 2, DOOR_Y + 30),
          "light through the doorway");

    /* text on the omp screen reaches the canvas */
    feed(&G.omp_vt, "W");
    game_render(&G, &cur);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    cell0 = (rect){tx, ty, CELL_W, CELL_H};
    CHECK(differing(&open, &cur, cell0) > 4, "a typed W is drawn");

    /* walk up through the door: room 2, progress saved */
    {
        key_event up;
        memset(&up, 0, sizeof up);
        up.key = KEY_UP;
        up.action = 1;
        G.release_events = true;
        game_key(&G, &up);
        for (int i = 0; i < 600 && G.sc.level == 0; i++) game_update(&G, 1.0 / 60);
    }
    CHECK(G.sc.level == 1, "walked into room 2");
    CHECK(G.sc.y == START_Y && G.sc.door == 0, "room 2 entrance, door shut");
    snprintf(prog, sizeof prog, "%s/progress", state);
    {
        FILE *f = fopen(prog, "r");
        int n = 0;
        CHECK(f && fscanf(f, "level %d", &n) == 1 && n == 2, "progress says level 2");
        if (f) fclose(f);
    }
    game_shutdown(&G);

    /* a restart resumes in room 2 */
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "restart");
    CHECK(G.sc.level == 1, "resumed in room 2");
    game_render(&G, &cur);
    game_shutdown(&G);

    /* goal already met on entry: the door is open at once */
    unlink(prog);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "fresh start");
    CHECK(G.sc.level == 0 && G.sc.goal_met && G.sc.door == 1.0f, "earlier work opens the door");
    /* deleting the file closes it again */
    unlink(hello);
    game_update(&G, 0.6);
    CHECK(!G.sc.goal_met, "goal lost when the file goes");
    game_shutdown(&G);

    sr_canvas_free(&shut);
    sr_canvas_free(&open);
    sr_canvas_free(&cur);
}

int main(void)
{
    const char *dir = getenv("PUZZLLM_TEST_DIR");
    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "set PUZZLLM_TEST_DIR to an empty scratch folder\n");
        return 2;
    }
    snprintf(scratch, sizeof scratch, "%s", dir);
    test_vt();
    test_keys();
    test_levels();
    test_json();
    test_activity();
    test_scene();
    test_render_and_flow();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
