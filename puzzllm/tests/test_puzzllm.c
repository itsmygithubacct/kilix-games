/* test_puzzllm.c - headless unit tests: terminal emulator, keys, levels,
 * JSON, activity feed, Kilix's pictures, the 60 Hz scene, layout at any pane
 * size, both renderers, rendering and the level-1 flow.
 *
 * PUZZLLM_TEST_DIR is a scratch folder the test may fill (make test-unit
 * empties it first). Run from the puzzllm/ directory.
 */
#include "activity.h"
#include "damage.h"
#include "draw.h"
#include "game.h"
#include "json.h"
#include "kilix_art.h"
#include "keys.h"
#include "layout.h"
#include "levels.h"
#include "render.h"
#include "scene.h"
#include "neuroterm_terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static void feed(neuroterm *t, const char *s) { neuroterm_feed(t, s, strlen(s)); }

static const char *row(const neuroterm *t, int y)
{
    static char buf[4096];
    neuroterm_row_text(t, y, buf, sizeof buf);
    return buf;
}

static void write_file(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror(path); exit(2); }
    if (n > 0 && write(fd, data, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
}

static bool file_contains(const char *path, const char *needle)
{
    char buf[1200];
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) return false;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static void sub(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", scratch, name);
}

/* ---------------- terminal emulator ---------------- */

static VTermScreenCell cell_at(const neuroterm *t, int col, int row)
{
    VTermScreenCell cell = {0};
    neuroterm_cell(t, col, row, &cell);
    return cell;
}

static void test_neuroterm(void)
{
    neuroterm t;
    char r[64];
    size_t n;

    CHECK(neuroterm_init(&t, 20, 5), "neuroterm_init");
    feed(&t, "hello\r\nworld");
    CHECK(strcmp(row(&t, 0), "hello") == 0, "row0 '%s'", row(&t, 0));
    CHECK(strcmp(row(&t, 1), "world") == 0, "row1 '%s'", row(&t, 1));
    CHECK(neuroterm_cursor(&t).col == 5 && neuroterm_cursor(&t).row == 1, "cursor %d,%d", neuroterm_cursor(&t).col, neuroterm_cursor(&t).row);

    feed(&t, "\033[3;5HX");
    CHECK(cell_at(&t, 4, 2).chars[0] == 'X', "CUP puts X at column 5 row 3");
    feed(&t, "\033[2J\033[H");
    CHECK(strcmp(row(&t, 0), "") == 0 && neuroterm_cursor(&t).col == 0 && neuroterm_cursor(&t).row == 0, "ED 2 + home");

    /* autowrap at the right edge */
    feed(&t, "abcdefghijklmnopqrstUV");
    CHECK(strcmp(row(&t, 0), "abcdefghijklmnopqrst") == 0, "wrap row0 '%s'", row(&t, 0));
    CHECK(strcmp(row(&t, 1), "UV") == 0, "wrap row1 '%s'", row(&t, 1));

    /* erase in line */
    feed(&t, "\033[1;3H\033[K");
    CHECK(strcmp(row(&t, 0), "ab") == 0, "EL 0 '%s'", row(&t, 0));

    /* colours */
    feed(&t, "\033[H\033[31mR\033[38;5;196mS\033[38;2;1;2;3mT\033[48:2::9:8:7mU\033[0mV\033[7mW");
    VTermScreenCell red = cell_at(&t, 0, 0);
    CHECK(VTERM_COLOR_IS_INDEXED(&red.fg) && red.fg.indexed.idx == 1, "SGR 31");
    CHECK(neuroterm_color(&t, cell_at(&t, 1, 0).fg) == 0xff0000, "SGR 38;5;196");
    CHECK(neuroterm_color(&t, cell_at(&t, 2, 0).fg) == 0x010203, "SGR 38;2 truecolour");
    CHECK(neuroterm_color(&t, cell_at(&t, 3, 0).bg) == 0x090807, "SGR 48:2:: colon form");
    feed(&t, "\033[H\033[38:2:4:5:6;1mZ\033[0m");
    CHECK(neuroterm_color(&t, cell_at(&t, 0, 0).fg) == 0x040506 && cell_at(&t, 0, 0).attrs.bold,
          "SGR 38:2:r:g:b then ;1");
    VTermScreenCell normal = cell_at(&t, 4, 0);
    CHECK(VTERM_COLOR_IS_DEFAULT_FG(&normal.fg), "SGR 0 resets");
    CHECK(cell_at(&t, 5, 0).attrs.reverse, "SGR 7");
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
    CHECK(strcmp(row(&t, 0), "") == 0, "1049 enters a blank alt screen");
    feed(&t, "alt!");
    feed(&t, "\033[?1049l");
    CHECK(strcmp(row(&t, 0), "main") == 0, "1049 restores main '%s'", row(&t, 0));
    CHECK(neuroterm_cursor(&t).col == 4 && neuroterm_cursor(&t).row == 0, "1049 restores the cursor");

    /* UTF-8, also split across feeds; box drawing; wide characters */
    feed(&t, "\033[2J\033[H\xe2\x94");
    feed(&t, "\x80\xe2\x95\xad");
    CHECK(cell_at(&t, 0, 0).chars[0] == 0x2500 && cell_at(&t, 1, 0).chars[0] == 0x256d,
          "split UTF-8");
    feed(&t, "\xe4\xb8\xad|");
    CHECK(cell_at(&t, 2, 0).chars[0] == 0x4e2d && cell_at(&t, 2, 0).width == 2,
          "wide char");
    CHECK(cell_at(&t, 4, 0).chars[0] == '|', "wide char takes two cells");
    feed(&t, "\xff");
    CHECK(cell_at(&t, 5, 0).chars[0] == 0xfffd, "invalid byte -> U+FFFD");

    /* replies */
    feed(&t, "\033[2;3H\033[6n\033[c");
    n = neuroterm_read(&t, r, sizeof r - 1);
    r[n] = '\0';
    CHECK(strncmp(r, "\033[2;3R\033[?", 9) == 0, "CPR + DA1 reply: %s", r);
    feed(&t, "\033[?u");
    CHECK(neuroterm_read(&t, r, sizeof r) == 0, "Kitty keyboard query is not answered");

    /* strings are swallowed: OSC title, APC (Kitty graphics), DCS */
    feed(&t, "\033[2J\033[H\033]0;my title\007\033_Gf=100;AAAA\033\\\033Pq#0\033\\ok");
    CHECK(strcmp(row(&t, 0), "ok") == 0, "strings swallowed '%s'", row(&t, 0));

    /* modes */
    feed(&t, "\033[?1h\033[?25l\033[?2004h");
    key_event up = {.key = KEY_UP, .action = 1};
    CHECK(!t.cursor_visible && neuroterm_key(&t, &up, r, sizeof r) == 3 &&
          memcmp(r, "\033OA", 3) == 0, "hidden cursor and application cursor encoding");
    feed(&t, "\033[?1l\033[?25h");
    CHECK(t.cursor_visible && neuroterm_key(&t, &up, r, sizeof r) == 3 &&
          memcmp(r, "\033[A", 3) == 0, "cursor modes reset");
    feed(&t, "\033[2J\033[H\033[3me\xcc\x81\033[0m");
    VTermScreenCell accent = cell_at(&t, 0, 0);
    CHECK(accent.attrs.italic && accent.chars[0] == 'e' && accent.chars[1] == 0x301,
          "NeuroTerm retains italic and combining marks in the cell");

    /* resize keeps the cursor line */
    feed(&t, "\033[2J\033[5;1Hlast");
    CHECK(neuroterm_resize(&t, 10, 3), "resize");
    CHECK(strcmp(row(&t, 2), "last") == 0 && neuroterm_cursor(&t).row == 2, "resize keeps bottom '%s'", row(&t, 2));
    neuroterm_free(&t);
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

static int run_script_check(const char *dir, const char *script_name)
{
    char checker[4096], cwd[4096];
    pid_t pid;
    int status;
    if (getcwd(cwd, sizeof cwd) == NULL) return 127;
    {
        int n = snprintf(checker, sizeof checker, "%s/libexec/puzzllm-script-check", cwd);
        if (n < 0 || (size_t)n >= sizeof checker) return 127;
    }
    pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (chdir(dir) != 0) _exit(127);
        execlp("timeout", "timeout", "--kill-after=1s", "3s",
               "bash", checker, script_name, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void test_script_and_stamp(void)
{
    level_list L;
    char err[256], ws[2300], path[2400], body[700];
    goal_result r;
    script_puzzle sp;
    const char *block =
        "level: 1\ntitle: t\ngoal: g\ncheck: script keep.sh\n"
        "level: 2\ntitle: u\ngoal: h\ncheck: stamped-log\n"
        "level: 3\ntitle: v\ngoal: i\ncheck: follow-log\n"
        "level: 4\ntitle: w\ngoal: j\ncheck: none\n";
    const char *keeper =
        "awk '{\n"
        "  for (i = 1; i <= NF; i++) {\n"
        "    n = $i + 0\n"
        "    if (seen && n > prev) print n\n"
        "    prev = n\n"
        "    seen = 1\n"
        "  }\n"
        "}'\n";
    const char *drops_negatives =
        "awk '{\n"
        "  for (i = 1; i <= NF; i++) {\n"
        "    n = $i + 0\n"
        "    if (seen && n > prev && n >= 0 && prev >= 0) print n\n"
        "    prev = n\n"
        "    seen = 1\n"
        "  }\n"
        "}'\n";
    static const struct { const char *id, *file; int stamp; } rows[] = {
        {"0123456789abcdef01234567", "inventory.csv", 4},
        {"fedcba9876543210fedcba98", "rooms.json", 4},
        {"00112233445566778899aabb", "assets.manifest", 2},
        {"abcdeffedcba0123456789ab", "routes.txt", 4},
    };

    CHECK(levels_parse(&L, block, err, sizeof err) && L.count == 4, "both checks parse: %s", err);
    CHECK(L.items[0].check == CHECK_SCRIPT && strcmp(L.items[0].file, "keep.sh") == 0,
          "script check names keep.sh");
    CHECK(L.items[1].check == CHECK_STAMPED_LOG && L.items[1].file[0] == '\0',
          "stamped-log stores no filename");
    CHECK(L.items[2].check == CHECK_FOLLOW_LOG, "follow-log still parses");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: script\n", err, sizeof err),
          "a script check needs a file name");
    CHECK(strstr(err, "plain, visible") != NULL, "script name error: %s", err);
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: script ../keep.sh\n", err, sizeof err),
          "a script path is rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: script .keep.sh\n", err, sizeof err),
          "a hidden script name is rejected");
    CHECK(levels_parse(&L, block, err, sizeof err), "reparse: %s", err);

    sub(ws, sizeof ws, "stamp-ws");
    mkdir(ws, 0700);
    r = levels_check(&L.items[1], ws);
    CHECK(!r.met && strstr(r.why, "run ./run_check.sh") != NULL, "missing log: %s", r.why);
    snprintf(path, sizeof path, "%s/shell.log", ws);
    snprintf(body, sizeof body,
             "noise\nPuzzLLM check failed: %s %s\n", rows[0].id, rows[0].file);
    write_file(path, body, strlen(body));
    r = levels_check(&L.items[1], ws);
    CHECK(!r.met && strstr(r.why, "stamp") != NULL, "missing stamped report: %s", r.why);

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int n = snprintf(body, sizeof body, "noise\nPuzzLLM check failed: %s %s\n",
                         rows[i].id, rows[i].file);
        snprintf(path, sizeof path, "%s/shell.log", ws);
        write_file(path, body, (size_t)n);
        n = snprintf(body, sizeof body, "%s %s %d\n", rows[i].id, rows[i].file, rows[i].stamp);
        snprintf(path, sizeof path, "%s/report.txt", ws);
        write_file(path, body, (size_t)n);
        r = levels_check(&L.items[1], ws);
        CHECK(r.met, "%s stamp %d: %s", rows[i].file, rows[i].stamp, r.why);
        n = snprintf(body, sizeof body, "%s %s %d\n", rows[i].id, rows[i].file, rows[i].stamp + 1);
        write_file(path, body, (size_t)n);
        CHECK(!levels_check(&L.items[1], ws).met, "%s rejects the next stamp", rows[i].file);
    }

    snprintf(path, sizeof path, "%s/shell.log", ws);
    snprintf(body, sizeof body,
             "PuzzLLM check failed: %s %s\nPuzzLLM check failed: %s %s\n",
             rows[3].id, rows[3].file, rows[0].id, rows[0].file);
    write_file(path, body, strlen(body));
    snprintf(path, sizeof path, "%s/report.txt", ws);
    snprintf(body, sizeof body, "%s %s %d\n", rows[0].id, rows[0].file, rows[0].stamp);
    write_file(path, body, strlen(body));
    r = levels_check(&L.items[1], ws);
    CHECK(r.met, "latest failure wins: %s", r.why);
    snprintf(body, sizeof body, "%s %s %d\n", rows[3].id, rows[3].file, rows[3].stamp);
    write_file(path, body, strlen(body));
    CHECK(!levels_check(&L.items[1], ws).met, "an older failure does not pass");
    snprintf(body, sizeof body, "%s %s\n", rows[0].id, rows[0].file);
    write_file(path, body, strlen(body));
    CHECK(!levels_check(&L.items[1], ws).met, "a two-field report is missing the stamp");
    r = levels_check(&L.items[2], ws);
    CHECK(r.met, "follow-log still accepts the two-field report: %s", r.why);

    sub(ws, sizeof ws, "script-ws");
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/keep.sh", ws);
    r = script_puzzle_check(".", ".", ws, "keep.sh");
    CHECK(!r.met && strstr(r.why, "regular file") != NULL, "missing script: %s", r.why);
    r = script_puzzle_check(".", ".", ws, "../keep.sh");
    CHECK(!r.met, "a script path is not launched: %s", r.why);
    memset(&sp, 0, sizeof sp);
    r = script_puzzle_poll(&sp, ".", ".", ws, "keep.sh");
    CHECK(!r.met && sp.grader == 0, "a missing script does not fork: %s", r.why);
    script_puzzle_stop(&sp);

    const char *sample_only = "printf '4\\n9\\n'\n";
    write_file(path, keeper, strlen(keeper));
    CHECK(run_script_check(ws, "keep.sh") == 0, "the keeper passes the hidden inputs");
    write_file(path, sample_only, strlen(sample_only));
    CHECK(run_script_check(ws, "keep.sh") == 12, "the sample output is not a fixture");
    write_file(path, drops_negatives, strlen(drops_negatives));
    CHECK(run_script_check(ws, "keep.sh") == 13, "dropping negatives fails");
    r = levels_check(&L.items[0], ws);
    CHECK(!r.met, "the host file check does not grade the script: %s", r.why);

    unlink(path);
    CHECK(symlink("other.sh", path) == 0, "script symlink");
    CHECK(run_script_check(ws, "keep.sh") == 11, "a symlink is not the script");
    unlink(path);
    {
        char *big = malloc(65537);
        CHECK(big != NULL, "size buffer");
        if (big != NULL) {
            memset(big, 'x', 65537);
            write_file(path, big, 65537);
            free(big);
            CHECK(run_script_check(ws, "keep.sh") == 11, "an oversized script is refused");
        }
    }
}

static void test_strawberry(void)
{
    level_list levels;
    char err[256], ws[2300], path[2400];
    CHECK(levels_load(&levels, "levels/levels.txt", err, sizeof err), "levels.txt: %s", err);
    const level *lv = &levels.items[1];
    CHECK(strcmp(lv->file, "strawberry.txt") == 0, "room 2 checks strawberry.txt");
    CHECK(strstr(levels.items[0].goal, "~/hello_world.txt") != NULL,
          "room 1 names the workspace destination");
    sub(ws, sizeof ws, "strawberry-ws");
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/strawberry.txt", ws);
    CHECK(!levels_check(lv, ws).met, "room 2 needs the answer file");
    write_file(path, "3", 1);
    CHECK(levels_check(lv, ws).met, "the bare count is correct");
    write_file(path, " \t3\r\n", 5);
    CHECK(levels_check(lv, ws).met, "the count may have surrounding whitespace");
    write_file(path, "2\n", 2);
    CHECK(!levels_check(lv, ws).met, "the wrong count is refused");
    write_file(path, "There are 3", 11);
    CHECK(!levels_check(lv, ws).met, "a substring is not the answer");
    write_file(path, "3\0x", 3);
    CHECK(!levels_check(lv, ws).met, "embedded NUL cannot hide extra content");
}

/* Exercise reusable phrase checks with a synthetic level. */
static void test_phrase_checks(void)
{
    static const char *good[] = {
        "To take your car to the carwash you must drive, because the car has to be at the carwash to be washed.",
        "You need to DRIVE to the car wash. Walking leaves the car at home, and the car must be there.",
        "Drive. The carwash is close, but you can't just walk there: the car needs to be at the car-wash so it can be washed.",
        "You have to drive it to the carwash since you must have the car at the carwash to wash it.",
        "Driving is the only way: you shouldn't walk, because the car would stay home instead of at the carwash.",
    };
    static const char *bad[][2] = {
        {"You should walk to the carwash because it is only 50 metres away.", "should walk"},
        {"It is better to walk to the car wash, since driving 50 m wastes fuel.", "better to walk"},
        {"Walk instead of driving to the carwash, because it is close.", "walk instead"},
        {"You must drive, because the car has to be there.", "none of: carwash"},
        {"Take the car to the carwash because it has to be washed.", "none of: drive"},
        {"Drive to the carwash.", "none of: because"},
        {"I recommend walking to the carwash; driving is pointless since it is near.", "recommend walking"},
    };
    level_list L;
    const level *lv;
    char err[256], ws[2300], path[2400];
    goal_result r;
    int wrong = 0;

    CHECK(levels_parse(&L, "level: 1\ntitle: Example\ngoal: Test phrases\ncheck: file carwash.txt\n"
                          "require: lacks-ci should walk | better to walk | recommend walking | walk instead\n"
                          "require: contains-any-ci carwash | car wash | car-wash\n"
                          "require: contains-any-ci drive | driving | drove\n"
                          "require: contains-any-ci because | since | so that | otherwise | has to be | must be | needs to be | need to be\n",
                          err, sizeof err), "phrase rules: %s", err);
    lv = &L.items[0];
    sub(ws, sizeof ws, "level2-ws");
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/carwash.txt", ws);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "does not exist"), "missing: %s", r.why);
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        write_file(path, good[i], strlen(good[i]));
        r = levels_check(lv, ws);
        if (!r.met) { wrong++; printf("  rejected: %s (%s)\n", good[i], r.why); }
    }
    CHECK(wrong == 0, "every right statement is accepted (%d rejected)", wrong);
    wrong = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        write_file(path, bad[i][0], strlen(bad[i][0]));
        r = levels_check(lv, ws);
        if (r.met || strstr(r.why, bad[i][1]) == NULL) {
            wrong++;
            printf("  accepted or misreported: %s (%s)\n", bad[i][0], r.why);
        }
    }
    CHECK(wrong == 0, "every wrong statement is refused, for its reason (%d not)", wrong);

    /* the new requires on their own */
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a\nrequire: lacks-ci a | | b\n",
                        err, sizeof err), "an empty alternative is rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a\n"
                            "require: contains-any-ci a|b|c|d|e|f|g|h|i|j|k|l|m\n", err, sizeof err),
          "too many alternatives are rejected");
    CHECK(levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a.txt\n"
                           "require: contains-any-ci Zebra\n", err, sizeof err), "one alternative: %s", err);
    snprintf(path, sizeof path, "%s/a.txt", ws);
    write_file(path, "a horse", 7);
    r = levels_check(&L.items[0], ws);
    CHECK(!r.met && strstr(r.why, "does not say \"Zebra\""), "one alternative, missing: %s", r.why);
    write_file(path, "a ZEBRA", 7);
    CHECK(levels_check(&L.items[0], ws).met, "any case");
}

/* Level 3: 1.txt holds exactly a million ones and nothing else. */
static void test_million_ones(void)
{
    enum { MILLION = 1000000 };
    level_list L;
    const level *lv;
    char err[256], ws[2300], path[2400];
    char *buf = malloc(2 * MILLION + 2);
    goal_result r;

    CHECK(buf != NULL, "buffer");
    if (buf == NULL) return;
    memset(buf, '1', 2 * MILLION + 2);
    CHECK(levels_load(&L, "levels/levels.txt", err, sizeof err), "levels.txt: %s", err);
    lv = &L.items[2];
    CHECK(L.count >= 4 && lv->check == CHECK_FILE && strcmp(lv->file, "1.txt") == 0,
          "level 3 checks 1.txt");
    CHECK(lv->nhints >= 4, "level 3 has hints");
    sub(ws, sizeof ws, "level3-ws");
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/1.txt", ws);

    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "does not exist"), "missing: %s", r.why);
    write_file(path, buf, MILLION);
    r = levels_check(lv, ws);
    CHECK(r.met, "a million ones: %s", r.why);

    buf[MILLION] = '\n';
    write_file(path, buf, MILLION + 1);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "1,000,001") && strstr(r.why, "line ending"),
          "a trailing newline fails, and says so: %s", r.why);
    buf[MILLION] = '1';
    write_file(path, buf, MILLION - 1);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "999,999 bytes; it needs exactly 1,000,000"),
          "one short: %s", r.why);
    write_file(path, buf, MILLION + 2);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "1,000,002 bytes; it needs exactly 1,000,000"),
          "two over: %s", r.why);
    write_file(path, buf, 2 * MILLION);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "2,000,000 bytes"), "far too many: %s", r.why);

    buf[499999] = ' ';
    write_file(path, buf, MILLION);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "a space at byte 500,000"), "a space in the middle: %s", r.why);
    buf[499999] = '1';
    buf[MILLION - 1] = '\n';
    write_file(path, buf, MILLION);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "a line ending at byte 1,000,000"),
          "999,999 ones and a newline: %s", r.why);
    buf[MILLION - 1] = '1';
    buf[0] = '0';
    write_file(path, buf, MILLION);
    r = levels_check(lv, ws);
    CHECK(!r.met && strstr(r.why, "'0' at byte 1"), "a zero first: %s", r.why);
    buf[0] = '1';

    /* a symlink to a good file does not count */
    {
        char real[2400];
        snprintf(real, sizeof real, "%s/real.txt", ws);
        write_file(real, buf, MILLION);
        unlink(path);
        CHECK(symlink(real, path) == 0, "symlink");
        CHECK(!levels_check(lv, ws).met, "a symlinked 1.txt does not count");
        unlink(path);
    }

    /* the new requires on their own */
    CHECK(levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a.txt\nrequire: only 1\n",
                       err, sizeof err), "only parses: %s", err);
    snprintf(path, sizeof path, "%s/a.txt", ws);
    write_file(path, "111\n", 4);
    r = levels_check(&L.items[0], ws);
    CHECK(!r.met && strstr(r.why, "a line ending at byte 4"), "only sees the last byte: %s", r.why);
    write_file(path, "1111", 4);
    CHECK(levels_check(&L.items[0], ws).met, "only: all ones");
    write_file(path, buf, 100000);
    r = levels_check(&L.items[0], ws);
    CHECK(!r.met && strstr(r.why, "too big"), "no size named: the 64 KiB limit holds: %s", r.why);
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a\nrequire: bytes 16777217\n",
                        err, sizeof err), "a size over 16 MiB is rejected");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a\nrequire: bytes 1e6\n",
                        err, sizeof err), "a size must be digits");
    CHECK(!levels_parse(&L, "level: 1\ntitle: t\ngoal: g\ncheck: file a\nrequire: only \n",
                        err, sizeof err), "only needs characters");
    free(buf);
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
        "{\"type\":\"session\",\"version\":3,\"id\":\"x\",\"cwd\":\"/kilix\"}",
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
    const char usage[] = "{\"type\":\"message\",\"message\":{\"role\":\"assistant\","
        "\"usage\":{\"input\":12,\"output\":9,\"cacheRead\":3,\"totalTokens\":24}}}";
    const char split_usage[] = "{\"type\":\"message\",\"message\":{\"role\":\"assistant\","
        "\"usage\":{\"input\":5,\"output\":2}}}";
    activity_session_line(&act, usage, strlen(usage));
    activity_session_line(&act, split_usage, strlen(split_usage));
    CHECK(act.tokens_reported && act.tokens_used == 31,
          "local token totals sum each response once, including reported cache usage");
    const char codex_usage[] = "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\","
        "\"info\":{\"total_token_usage\":{\"total_tokens\":57}}}}";
    act.codex = true;
    activity_session_line(&act, codex_usage, strlen(codex_usage));
    activity_session_line(&act, codex_usage, strlen(codex_usage));
    CHECK(act.tokens_used == 57, "Codex cumulative events replace the previous total");

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
    int ticks = (int)(seconds * SCENE_TICK_HZ + 0.5f);
    for (int i = 0; i < ticks; i++) {
        scene_event e = scene_tick(s, dx, dy);
        if (e != SCENE_NONE) last = e;
        if (e == SCENE_NEXT_ROOM || e == SCENE_BOTTOM_EXIT) break;
    }
    return last;
}

static void test_scene(void)
{
    scene s;
    scene_init(&s, 0, 2);
    CHECK(scene_walkable(&s, START_X, START_Y), "start is walkable");
    CHECK(!scene_walkable(&s, 300, 300), "screens are not floor");

    for (int i = SCREEN_OMP; i <= SCREEN_SHELL; i++) {
        rect pane = layout_screen((screen_id)i);
        float x = pane.x + pane.w / 2.0f;
        CHECK(!scene_walkable(&s, x, WALK_Y0), "pane %d blocks feet even in the floor band", i);
        int bottom = pane.y + pane.h + SCREEN_BEZEL;
        CHECK(scene_walkable(&s, x, bottom) && !scene_walkable(&s, x, bottom - 0.25f),
              "pane %d blocks movement exactly at the bezel", i);
        s.x = x;
        s.y = (float)bottom;
        walk(&s, 0, -1, 1);
        CHECK(s.y == bottom,
              "walking up stops at pane %d's bezel", i);
    }
    scene_enter_room(&s, 0);

    /* door shut: walking up the aisle stops at the threshold */
    CHECK(walk(&s, 0, -1, 5) != SCENE_NEXT_ROOM, "shut door blocks");
    CHECK(s.y >= AISLE_Y0 - 0.01f, "stopped at the threshold (y=%.1f)", s.y);
    CHECK(s.door == 0, "door shut");

    /* near screens */
    s.x = 200; s.y = WALK_Y0; s.dir = 1;
    CHECK(scene_near_screen(&s) == SCREEN_OMP, "below the full-height coding agent");
    s.x = 640; s.y = 200; s.dir = 3;
    CHECK(scene_near_screen(&s) == SCREEN_GOAL, "aisle facing right: goal and hints");
    s.dir = 2;
    CHECK(scene_near_screen(&s) == SCREEN_OMP, "aisle facing left: omp");
    s.x = 1000; s.y = WALK_Y0; s.dir = 1;
    CHECK(scene_near_screen(&s) == SCREEN_SHELL, "below the shell");
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

    scene_enter_room(&s, 1);
    CHECK(walk(&s, 0, 1, 1) == SCENE_BOTTOM_EXIT, "walking down the carpet reaches the bottom exit");
    scene_enter_room(&s, 1);
    s.x = AISLE_X1 + 20;
    CHECK(walk(&s, 0, 1, 1) != SCENE_BOTTOM_EXIT, "off the carpet the bottom wall blocks movement");

    /* typing into a screen freezes Kilix */
    scene_enter_room(&s, 0);
    s.focus = FOCUS_OMP;
    walk(&s, 1, 0, 1);
    CHECK(s.x == START_X, "no walking while typing");
}


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

/* ---------------- Kilix's pictures (from Zell) ---------------- */

static void test_kilix_art(void)
{
    for (int i = 0; i < KILIX_ART_COUNT; i++) {
        const kilix_picture *p = kilix_art_get(i);
        int opaque = 0, bad = 0, flame = 0;
        CHECK(p != NULL, "picture %d unpacks", i);
        if (p == NULL) continue;
        CHECK(p->height == KILIX_ART_HEIGHT && p->width >= 50 && p->width <= 70,
              "picture %d is %dx%d", i, p->width, p->height);
        for (int k = 0; k < p->width * p->height; k++) {
            uint32_t px = p->pixels[k], a = px >> 24;
            if (a >= 200) opaque++;
            if (px == 0xffdc471d || px == 0xff9b29) flame++;
            if (((px >> 16) & 255) > a || ((px >> 8) & 255) > a || (px & 255) > a) bad++;
        }
        CHECK(opaque > 1000, "picture %d has a body (%d opaque pixels)", i, opaque);
        CHECK(bad == 0, "picture %d is premultiplied (%d pixels are not)", i, bad);
        int dir = i < KILIX_ART_WALK ? i : (i - KILIX_ART_WALK) / 4;
        bool shield_face = dir == 0 || dir == 2 || (dir == 3 && i >= KILIX_ART_WALK);
        CHECK(shield_face ? flame > 12 : flame == 0,
              "picture %d shows the flame only when the shield face is visible", i);
        CHECK(kilix_art_get(i) == p, "picture %d is unpacked once", i);
    }
    CHECK(kilix_art_get(-1) == NULL && kilix_art_get(KILIX_ART_COUNT) == NULL, "bad ids");
    /* the four directions look different */
    {
        const kilix_picture *down = kilix_art_get(KILIX_ART_STAND + 0);
        const kilix_picture *up = kilix_art_get(KILIX_ART_STAND + 1);
        int differ = 0, w = down->width < up->width ? down->width : up->width;
        for (int y = 0; y < KILIX_ART_HEIGHT; y++)
            for (int x = 0; x < w; x++)
                if (down->pixels[y * down->width + x] != up->pixels[y * up->width + x]) differ++;
        CHECK(differ > 500, "facing down and up differ (%d pixels)", differ);
    }

    /* Zell's timing: four walk frames of seven ticks, then round again */
    for (int dir = 0; dir < 4; dir++) {
        CHECK(kilix_pose_at(dir, 0, 0).art == KILIX_ART_STAND + dir, "standing, dir %d", dir);
        CHECK(kilix_pose_at(dir, 1, 0).art == KILIX_ART_WALK + dir * 4, "first step, dir %d", dir);
        CHECK(kilix_pose_at(dir, 7, 0).art == KILIX_ART_WALK + dir * 4, "tick 7 still frame 0");
        CHECK(kilix_pose_at(dir, 8, 0).art == KILIX_ART_WALK + dir * 4 + 1, "tick 8 is frame 1");
        CHECK(kilix_pose_at(dir, 22, 0).art == KILIX_ART_WALK + dir * 4 + 3, "tick 22 is frame 3");
        CHECK(kilix_pose_at(dir, 29, 0).art == KILIX_ART_WALK + dir * 4, "tick 29 wraps to frame 0");
    }
    CHECK(kilix_pose_at(0, 0, 0).offset_y == 0 && kilix_pose_at(0, 0, 11).offset_y == -1 &&
          kilix_pose_at(0, 0, 33).offset_y == 0, "standing bob");
    CHECK(kilix_pose_at(0, 5, 11).offset_y == 0, "no bob while walking");
}

/* ---------------- the 60 Hz tick ---------------- */

static void test_ticks(void)
{
    char err[256], state[2300];
    scene s;
    float x0, y0;

    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    scene_init(&s, 0, 2);
    x0 = s.x;
    scene_tick(&s, 1, 0);
    CHECK(fabsf(s.x - x0 - SCENE_WALK_SPEED) < 1e-4f && s.dir == 3 && s.step_ticks == 1,
          "a tick walks 3.5 px (moved %.2f)", s.x - x0);
    CHECK(fabsf(SCENE_WALK_SPEED - 3.5f) < 1e-6f, "Zell's walking pace");
    x0 = s.x; y0 = s.y;
    scene_tick(&s, -1, -1);
    CHECK(fabsf(hypotf(s.x - x0, s.y - y0) - SCENE_WALK_SPEED) < 1e-3f,
          "diagonals are not faster (%.3f)", hypotf(s.x - x0, s.y - y0));
    CHECK(s.dir == 2, "sideways wins when facing a diagonal");
    CHECK(s.step_ticks == 2, "still walking");
    scene_tick(&s, 0, 0);
    CHECK(s.step_ticks == 0 && !s.moving, "standing still resets the walk");
    {
        int idle = s.idle_ticks;
        scene_tick(&s, 0, 0);
        CHECK(s.idle_ticks == idle + 1, "the idle clock runs");
    }
    scene_enter_room(&s, 0);
    s.goal_met = true;
    for (int i = 0; i < SCENE_DOOR_TICKS - 1; i++) scene_tick(&s, 0, 0);
    CHECK(s.door < 1.0f, "door still opening after %d ticks", SCENE_DOOR_TICKS - 1);
    CHECK(scene_tick(&s, 0, 0) == SCENE_DOOR_OPENED, "door open on tick %d", SCENE_DOOR_TICKS);

    /* the game runs ticks from elapsed time, the same at any frame rate */
    sub(state, sizeof state, "ticks");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "game_init: %s", err);
    {
        key_event right;
        memset(&right, 0, sizeof right);
        right.key = KEY_RIGHT;
        right.action = 1;
        G.release_events = true;
        x0 = G.sc.x;
        game_key(&G, &right);
        game_update(&G, 0.1);                       /* one slow frame */
        CHECK(fabsf(G.sc.x - x0 - 6 * SCENE_WALK_SPEED) < 0.01f,
              "0.1 s is six ticks (moved %.2f)", G.sc.x - x0);
        x0 = G.sc.x;
        for (int i = 0; i < 30; i++) game_update(&G, 1.0 / 300);   /* fast frames */
        CHECK(fabsf(G.sc.x - x0 - 6 * SCENE_WALK_SPEED) < 0.01f,
              "thirty 300 Hz frames are six ticks too (moved %.2f)", G.sc.x - x0);
        x0 = G.sc.x;
        game_update(&G, 5.0);                       /* a stall */
        CHECK(G.sc.x - x0 <= 15 * SCENE_WALK_SPEED + 0.01f,
              "a stall runs at most a quarter second (moved %.2f)", G.sc.x - x0);
        /* held, Kilix keeps walking every tick until the key comes up */
        G.sc.x = (float)START_X; G.sc.y = (float)START_Y;
        x0 = G.sc.x;
        for (int i = 0; i < 60; i++) game_update(&G, 1.0 / 60);
        CHECK(G.sc.step_ticks >= 60 && fabsf(G.sc.x - x0 - 60 * SCENE_WALK_SPEED) < 0.01f,
              "a held key walks every tick (moved %.2f, %d ticks)", G.sc.x - x0, G.sc.step_ticks);
        right.action = 3;
        game_key(&G, &right);
        x0 = G.sc.x;
        game_update(&G, 1.0 / 60);
        CHECK(G.sc.x == x0, "the release stops him on the next tick");
    }
    game_shutdown(&G);
}

/* ---------------- layout for the pane ---------------- */

static bool overlaps(rect a, rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_layout(void)
{
    static const int sizes[][2] = {
        {960, 600}, {1024, 640}, {1280, 800}, {1366, 768}, {1600, 1000},
        {1908, 972}, {1920, 1080}, {2048, 1280}, {1000, 1200}, {2000, 620}};
    char err[256], state[2300];

    layout_set(1280, 800);
    CHECK(OMP_COLS == 70 && OMP_ROWS == 43 && SHELL_COLS == 70 && SHELL_ROWS == 20 &&
          ACT_ROWS == 7 && GOAL_ROWS == 6, "1280x800 fits four panels");
    CHECK(START_X == 640 && START_Y == 746 && AISLE_X0 == 620 && AISLE_X1 == 660 &&
          WALK_Y0 == 680 && WALK_Y1 == 768 && DOOR_X == 600, "1280x800 floor and aisle");
    layout_set(300, 200);
    CHECK(CANVAS_W == LAYOUT_MIN_W && CANVAS_H == LAYOUT_MIN_H, "small panes: the minimum");
    layout_set(5000, 4000);
    CHECK(CANVAS_W == LAYOUT_MAX_W && CANVAS_H == LAYOUT_MAX_H, "huge panes: the maximum");

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int w = sizes[i][0], h = sizes[i][1];
        rect omp, shell, act, goal, aisle, resources;
        scene s;
        layout_set(w, h);
        omp = layout_screen(SCREEN_OMP);
        shell = layout_screen(SCREEN_SHELL);
        act = layout_screen(SCREEN_ACT);
        goal = layout_screen(SCREEN_GOAL);
        resources = layout_resources();
        aisle = (rect){AISLE_X0, WALL_H, AISLE_X1 - AISLE_X0, FLOOR_Y - WALL_H};
        CHECK(CANVAS_W == w && CANVAS_H == h, "%dx%d is drawn at its own size", w, h);
        CHECK(omp.x >= 0 && act.x + act.w <= w && shell.x + shell.w <= w &&
              goal.x + goal.w <= w && shell.y + shell.h >= HUD_Y - 68 &&
              shell.y + shell.h < HUD_Y &&
              act.y + act.h <= FLOOR_Y && omp.y < 24 && omp.y + omp.h >= HUD_Y - 44 &&
              omp.y + omp.h < HUD_Y && goal.y == omp.y,
              "%dx%d: screens inside the room", w, h);
        CHECK(!overlaps(omp, aisle) && !overlaps(shell, aisle) &&
              !overlaps(act, aisle) && !overlaps(goal, aisle) &&
              goal.y + goal.h <= act.y && act.y + act.h <= resources.y &&
              resources.y + resources.h <= shell.y && resources.x == shell.x &&
              goal.x == act.x && goal.x == shell.x && omp.x < goal.x,
              "%dx%d: four separate screens clear of the aisle", w, h);
        CHECK(OMP_COLS >= 20 && OMP_COLS <= SCREEN_MAX_COLS && SHELL_ROWS >= 6 &&
              OMP_ROWS >= 10 && ACT_ROWS >= 2 && GOAL_ROWS >= 2,
              "%dx%d: usable terminals (%dx%d, %dx%d)", w, h, OMP_COLS, OMP_ROWS,
              SHELL_COLS, SHELL_ROWS);
        CHECK(WALK_Y1 < HUD_Y && WALK_Y0 > FLOOR_Y && DOOR_X + DOOR_W / 2 == w / 2,
              "%dx%d: floor band and door", w, h);
        scene_init(&s, 0, 2);
        CHECK(scene_walkable(&s, START_X, START_Y), "%dx%d: entrance walkable", w, h);
        for (int b = 0; b < ROOM_BUTTON_COUNT; b++) {
            rect button = layout_room_button((room_button)b);
            float x = button.x + button.w / 2.0f, y = button.y + button.h / 2.0f;
            CHECK(scene_walkable(&s, x, y) && scene_room_button_at(x, y) == (room_button)b &&
                  !overlaps(button, shell), "%dx%d: button %d is reachable below the shell", w, h, b);
        }
    }
    layout_set(1908, 972);
    CHECK(OMP_COLS > 100 && OMP_ROWS >= 29, "a big pane gets big terminals (%dx%d)",
          OMP_COLS, OMP_ROWS);

    /* resizing the game resizes the terminals and keeps Kilix on the floor */
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    sub(state, sizeof state, "resize");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "game_init: %s", err);
    G.sc.x = 300;
    float height_from_bottom = CANVAS_H - G.sc.y;
    CHECK(game_resize(&G, 1908, 972), "resize to 1908x972");
    CHECK(G.omp_vt.cols == OMP_COLS && G.omp_vt.rows == OMP_ROWS &&
          G.shell_vt.cols == SHELL_COLS && G.shell_vt.rows == SHELL_ROWS,
          "terminals follow the screens (%dx%d)", G.omp_vt.cols, G.omp_vt.rows);
    float resize_y = CANVAS_H - height_from_bottom;
    if (!scene_walkable(&G.sc, G.sc.x, resize_y)) resize_y = START_Y;
    CHECK(scene_walkable(&G.sc, G.sc.x, G.sc.y) && G.sc.y == resize_y &&
          fabsf(G.sc.x - (300 + 954 - 640)) < 0.01f,
          "Kilix keeps his place on the floor (%.0f, %.0f)", G.sc.x, G.sc.y);
    CHECK(!game_resize(&G, 1908, 972), "the same size again changes nothing");
    {
        sr_canvas big, again;
        CHECK(sr_canvas_init(&big, CANVAS_W, CANVAS_H) && sr_canvas_init(&again, CANVAS_W, CANVAS_H),
              "canvases");
        game_render(&G, &big);
        CHECK(at(&big, CANVAS_W - 20, FLOOR_Y + 40) != 0, "the floor reaches the far side");
        game_resize(&G, 1280, 800);
        {
            /* the room drawn for the smaller pane is that pane's room: its
             * HUD strip is at its own bottom, its far wall at its own edge */
            sr_canvas small;
            CHECK(sr_canvas_init(&small, CANVAS_W, CANVAS_H), "canvas");
            game_render(&G, &small);
            CHECK(at(&small, 4, CANVAS_H - 4) == at(&big, 4, big.h - 4) &&
                  at(&small, CANVAS_W - 3, 300) == at(&big, big.w - 3, 300),
                  "the 1280x800 room is drawn for 1280x800");
            sr_canvas_free(&small);
        }
        game_resize(&G, 1908, 972);
        game_render(&G, &again);
        CHECK(memcmp(big.px, again.px, (size_t)big.w * big.h * 4) == 0,
              "after a round trip the frame is the same");
        sr_canvas_free(&big);
        sr_canvas_free(&again);
    }
    game_resize(&G, LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    game_shutdown(&G);
}


/* ---------------- what changed between frames ---------------- */

static bool covered(const damage_rect *r, int n, int x, int y)
{
    for (int i = 0; i < n; i++)
        if (x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1) return true;
    return false;
}

static void test_damage(void)
{
    enum { W = 1908, H = 990 };
    uint32_t *prev = calloc((size_t)W * H, 4), *cur = calloc((size_t)W * H, 4);
    damage_rect r[DAMAGE_MAX_RECTS];
    int n, missed = 0;
    unsigned seed = 12345;

    CHECK(prev && cur, "buffers");
    if (!prev || !cur) return;
    CHECK(damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS) == 0, "nothing changed: no rects");

    cur[40 * W + 100] = 0xffffffff;
    n = damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS);
    CHECK(n == 1 && r[0].x0 == 64 && r[0].x1 == 128 && r[0].y0 == 32 && r[0].y1 == 64,
          "one pixel: its tile (%d rects, %d,%d-%d,%d)", n, r[0].x0, r[0].y0, r[0].x1, r[0].y1);
    CHECK(damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS) == 0, "sent once, then nothing");

    /* the ragged right edge and the bottom edge */
    cur[(H - 1) * W + (W - 1)] = 0xff00ff00;
    n = damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS);
    CHECK(n == 1 && covered(r, n, W - 1, H - 1) && r[0].x1 == W && r[0].y1 == H,
          "the corner pixel past the last whole tile (%d rects, x1 %d y1 %d)", n,
          n ? r[0].x1 : -1, n ? r[0].y1 : -1);

    /* Kilix walking down: one tall rectangle, not one per tile row */
    for (int y = 300; y < 500; y++) cur[y * W + 700] ^= 0x00ffffffu;
    n = damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS);
    CHECK(n == 1 && r[0].y0 == 288 && r[0].y1 == 512, "a column merges into one rect (%d)", n);

    /* too many separate changes: one rectangle around them all */
    for (int ty = 0; ty < H; ty += 2 * DAMAGE_TILE_H)
        for (int tx = 0; tx < W; tx += 2 * DAMAGE_TILE_W) cur[ty * W + tx] ^= 0x00ffffffu;
    n = damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS);
    CHECK(n == 1 && r[0].x0 == 0 && r[0].y0 == 0, "overflow: one rect around it all (%d)", n);
    CHECK(memcmp(prev, cur, (size_t)W * H * 4) == 0, "...and nothing was left unsent");

    /* any change at all is covered, and remembered */
    for (int round = 0; round < 20; round++) {
        int k = 1 + round % 7;
        int xs[8], ys[8];
        for (int i = 0; i < k; i++) {
            seed = seed * 1103515245u + 12345u;
            xs[i] = (int)((seed >> 8) % W);
            seed = seed * 1103515245u + 12345u;
            ys[i] = (int)((seed >> 8) % H);
            cur[ys[i] * W + xs[i]] ^= 0x00123456u;
        }
        n = damage_update(prev, cur, W, H, r, DAMAGE_MAX_RECTS);
        for (int i = 0; i < k; i++)
            if (!covered(r, n, xs[i], ys[i])) missed++;
        if (memcmp(prev, cur, (size_t)W * H * 4) != 0) missed++;
    }
    CHECK(missed == 0, "random changes are all covered and remembered (%d missed)", missed);
    free(prev);
    free(cur);
}

/* ---------------- both renderers ---------------- */

static int ink(const sr_canvas *c, rect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            if (at(c, x, y) != 0) n++;
    return n;
}

static void draw_sample(sr_canvas *c)
{
    const kilix_picture *k = kilix_art_get(KILIX_ART_STAND);
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    draw_text(c, 0, 0, "Hello", 0xffffff, 1.0f);
    draw_glyph(c, 0, 16, 8, 16, 0x256d, 0xffffff, false);       /* box corner */
    draw_glyph(c, 8, 16, 8, 16, 0x2847, 0xffffff, false);       /* braille */
    draw_picture(c, k->pixels, k->width, k->height, 50, 0, k->width, k->height, 1.0f);
    draw_disc(c, 150, 40, 17, 5, 0xffffff, 1.0f);
    draw_stroke(c, 130, 60, 40, 20, 1, 0xffffff, 1.0f);
}

static void test_draw(void)
{
    sr_canvas raster, neural;
    bool have = draw_init();
    rect text = {0, 0, 40, 16}, cell = {0, 16, 8, 16}, dots = {8, 16, 8, 16};
    rect pic = {50, 0, 64, 84}, disc = {130, 34, 40, 12};

    CHECK(sr_canvas_init(&raster, 200, 100) && sr_canvas_init(&neural, 200, 100), "canvases");
    draw_force_raster(true);
    CHECK(strcmp(draw_renderer(), "raster") == 0, "forced raster says so");
    draw_sample(&raster);
    CHECK(ink(&raster, text) > 40 && ink(&raster, cell) > 4 && ink(&raster, dots) > 4 &&
          ink(&raster, pic) > 1000 && ink(&raster, disc) > 50,
          "raster draws text, box, braille, Kilix and a disc");
    draw_force_raster(false);
    printf("renderer: %s\n", draw_renderer());
#ifdef PUZZLLM_TEST_NEURONTE
    CHECK(have, "built with Neuronte, so its weights must open here (%s)", draw_renderer());
#endif
    if (have) {
        CHECK(strncmp(draw_renderer(), "neuronte", 8) == 0, "Neuronte is in use");
        draw_sample(&neural);
        CHECK(ink(&neural, text) > 40 && ink(&neural, cell) > 4 && ink(&neural, dots) > 4 &&
              ink(&neural, pic) > 1000 && ink(&neural, disc) > 50,
              "Neuronte draws text, box, braille, Kilix and a disc");
        CHECK(differing(&raster, &neural, text) > 20, "the nets drew the text, not the 8x16 font");
        /* at his own size each octant sub-cell is one pixel, so the cells
         * must give back the picture pixel for pixel; a wrong mask shows */
        CHECK(differing(&raster, &neural, pic) == 0,
              "the octant cells reproduce Kilix exactly (%d pixels off)",
              differing(&raster, &neural, pic));
    }
    sr_canvas_free(&raster);
    sr_canvas_free(&neural);
}

/* omp's startup row: pi, tab, shift, play, hexagon, and a two-cell folder. */
static int raw_changed(const sr_canvas *c, int x, int y, int w, int h, uint32_t bg)
{
    int n = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (c->px[yy * c->w + xx] != bg) n++;
    return n;
}

static int high_alpha(const sr_canvas *c, int x, int y, int w, int h)
{
    int n = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if ((c->px[yy * c->w + xx] >> 24) >= 200) n++;
    return n;
}

static void test_omp_glyphs(void)
{
#ifdef PUZZLLM_TEST_NEURONTE
    static const uint32_t scalar[] = {0x03c0u, 0x21e5u, 0x21e7u, 0x25b6u, 0x2b22u, 0x1f4c1u};
    static const char *name[] = {"pi", "tab", "shift", "play", "hexagon", "folder"};
    static const int col[] = {0, 16, 32, 48, 64, 80, 96};
    static const uint32_t row[] = {0x03c0u, 0x21e5u, 0x21e7u, 0x25b6u, 0x2b22u, 0x1f4c1u, (uint32_t)'|'};
    sr_canvas dark, light;
    int i, differ;
    draw_force_raster(false);
    if (!draw_init()) {
        CHECK(false, "omp glyphs: Neuronte weights open");
        return;
    }
    for (i = 0; i < 6; i++)
        CHECK(draw_glyph_trained(scalar[i]) == 1, "omp glyph %s is trained", name[i]);
    if (!sr_canvas_init(&dark, 112, 16) || !sr_canvas_init(&light, 112, 16)) {
        CHECK(false, "omp glyph canvases");
        return;
    }
    memset(dark.px, 0, (size_t)dark.w * (size_t)dark.h * 4);
    for (i = 0; i < light.w * light.h; i++) light.px[i] = 0xffffffffu;
    for (i = 0; i < 7; i++) {
        int w = row[i] == 0x1f4c1u ? 16 : 8;
        draw_glyph(&dark, col[i], 0, w, 16, row[i], 0xffffffu, 0);
        draw_glyph(&light, col[i], 0, w, 16, row[i], 0x000000u, 0);
    }
    differ = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (dark.px[y * dark.w + 16 + x] != dark.px[y * dark.w + 32 + x]) differ++;
    CHECK(differ > 8, "tab and shift arrows differ (%d)", differ);
    CHECK(raw_changed(&dark, 80, 0, 8, 16, 0) > 4 && raw_changed(&dark, 88, 0, 8, 16, 0) > 4,
          "folder paints both cells");
    differ = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (dark.px[y * dark.w + 80 + x] != dark.px[y * dark.w + 88 + x]) differ++;
    CHECK(differ > 0, "folder halves are not the same cell twice");
    CHECK(raw_changed(&dark, 96, 0, 8, 16, 0) > 4, "the marker after the folder starts at the next column");
    CHECK(raw_changed(&dark, 8, 0, 8, 16, 0) == 0 && raw_changed(&dark, 104, 0, 8, 16, 0) == 0,
          "glyphs stay inside their columns");
    CHECK(high_alpha(&dark, 48, 0, 8, 16) > 0 && high_alpha(&dark, 64, 0, 8, 16) > 0,
          "play and hexagon have crisp ink");
    CHECK(raw_changed(&dark, 0, 0, 112, 16, 0) > 20 && raw_changed(&light, 0, 0, 112, 16, 0xffffffffu) > 20,
          "the row is visible on dark and light");
    for (i = 0; i < 6; i++) {
        int w = scalar[i] == 0x1f4c1u ? 16 : 8;
        CHECK(raw_changed(&dark, col[i], 0, w, 16, 0) > 4, "dark ink for %s", name[i]);
        CHECK(raw_changed(&light, col[i], 0, w, 16, 0xffffffffu) > 4, "light ink for %s", name[i]);
    }
    sr_canvas_free(&dark);
    sr_canvas_free(&light);
#else
    (void)0;
#endif
}

/* ---------------- rendering ---------------- */


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
    snprintf(prog, sizeof prog, "%s/workspace/GOAL.md", state);
    CHECK(file_contains(prog, "# Goal") && file_contains(prog, "Hello, World"),
          "room 1 writes its goal to ~/GOAL.md");
    snprintf(prog, sizeof prog, "%s/workspace/HINTS.md", state);
    CHECK(G.hints_file && file_contains(prog, "# Hints") &&
          file_contains(prog, "1. Walk to the agent") &&
          file_contains(prog, "5. You may also finish the goal yourself"),
          "room 1 writes every hint to ~/HINTS.md");

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
          "the opening behind the door is visible");

    CHECK(differing(&shut, &open, (rect){DOOR_X, 17, DOOR_W, 16}) > 0 &&
          differing(&shut, &open, (rect){DOOR_X, 0, DOOR_W, 16}) == 0,
          "the lock status changes underneath the room number above the door");
    CHECK(at(&shut, START_X, CANVAS_H - 1) != at(&shut, AISLE_X0 - 12, CANVAS_H - 1),
          "the carpet reaches the bottom edge");

    /* text on the omp screen reaches the canvas */
    feed(&G.omp_vt, "W");
    game_render(&G, &cur);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    cell0 = (rect){tx, ty, CELL_W, CELL_H};
    CHECK(differing(&open, &cur, cell0) > 4, "a typed W is drawn");

    char *idle[] = {"/bin/cat", NULL};
    CHECK(pty_spawn(&G.omp, idle, NULL, OMP_COLS, OMP_ROWS), "start a test agent");
    CHECK(pty_spawn(&G.shell, idle, NULL, SHELL_COLS, SHELL_ROWS), "start a test shell");
    pid_t agent_pid = G.omp.pid, shell_pid = G.shell.pid;
    G.ranked_attempt = (score_entry){.level = 1, .id = 110, .measure = {1250, 2300, 75, 10485760, -1, -1, 0},
                                     .model = "Ollama/test", .player = "Player"};
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
    snprintf(prog, sizeof prog, "%s/workspace/GOAL.md", state);
    CHECK(file_contains(prog, "strawberry") && !file_contains(prog, "Hello, World"),
          "room 2 replaces ~/GOAL.md with its goal");
    snprintf(prog, sizeof prog, "%s/workspace/HINTS.md", state);
    CHECK(file_contains(prog, "count every r") && !file_contains(prog, "Walk to the agent"),
          "room 2 replaces ~/HINTS.md with its hints");
    score_entry *scores = NULL;
    int score_count = 0;
    CHECK(leaderboard_load(state, &scores, &score_count) && score_count == 1 &&
          scores[0].measure[SCORE_TOKENS] == 1250,
          "finishing a measured room records its token total");
    free(scores);
    CHECK(agent_pid > 0 && G.omp.pid == 0 && waitpid(agent_pid, NULL, WNOHANG) < 0 &&
          errno == ECHILD, "leaving the room stops and reaps its agent");
    CHECK(shell_pid > 0 && G.shell.pid == shell_pid && !pty_poll_exit(&G.shell),
          "the shell survives the room change");
    CHECK(strcmp(row(&G.omp_vt, 0), "") == 0, "the old agent screen is cleared");
    CHECK(!has_line(&G.act, "GOAL MET"), "room 2 clears room 1 activity");
    snprintf(prog, sizeof prog, "%s/workspace/level_1/hello_world.txt", state);
    CHECK(access(hello, F_OK) != 0 && access(prog, F_OK) == 0,
          "room 1 file moves into its archive");
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
    char room_file[2400];
    snprintf(room_file, sizeof room_file, "%s/workspace/room-two-notes.txt", state);
    write_file(room_file, "keep", 4);
    key_event down = {.key = KEY_DOWN, .action = 1};
    G.release_events = true;
    game_key(&G, &down);
    for (int i = 0; i < 120 && G.sc.level == 1; i++) game_update(&G, 1.0 / 60);
    CHECK(G.sc.level == 0 && G.sc.y > DOOR_EXIT_Y && G.sc.y < WALK_Y0 &&
          game_load_progress(state, G.levels.count) == 0 && G.sc.goal_met &&
          access(hello, F_OK) == 0 && access(room_file, F_OK) != 0,
          "bottom exit restores the previous room at its door and archives the current room");
    key_event up = {.key = KEY_UP, .action = 1};
    game_key(&G, &up);
    for (int i = 0; i < 120 && G.sc.level == 0; i++) game_update(&G, 1.0 / 60);
    CHECK(G.sc.level == 1 && access(room_file, F_OK) == 0, "revisiting room 2 restores its work");
    game_key(&G, &down);
    for (int i = 0; i < 120 && G.sc.level == 1; i++) game_update(&G, 1.0 / 60);
    G.sc.y = CANVAS_H - 4;
    game_key(&G, &down);
    for (int i = 0; i < 10 && !G.quit; i++) game_update(&G, 1.0 / 60);
    CHECK(G.quit && G.sc.level == 0 && access(hello, F_OK) == 0,
          "walking out of room 1 quits with its current files saved");
    game_shutdown(&G);

    /* goal already met on entry: the door is open at once */
    unlink(prog);
    write_file(hello, "Hello, World!\n", 14);
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


static void menu_press(uint32_t key)
{
    key_event ev = {.key = key, .action = 1};
    if (key == KEY_ESCAPE && !G.menu.open && G.sc.focus != FOCUS_ROOM) {
        ev.key = KEY_F12;
        ev.mods = MOD_CTRL | MOD_SHIFT;
    }
    game_key(&G, &ev);
}

static int menu_item_page(int item)
{
    for (int page = MENU_PAGE_MAIN; page <= MENU_PAGE_RESET; page++) {
        int rows[12];
        int n = menu_page_rows(page, draw_neuronte_available(), rows, 12);
        for (int i = 0; i < n; i++) if (rows[i] == item) return page;
    }
    return MENU_PAGE_MAIN;
}

static void menu_to(int item)
{
    if (!G.menu.open) menu_press(KEY_ESCAPE);
    if (G.menu.prompt_edit || G.menu.details_open || G.menu.leaderboard_open ||
        G.menu.confirm_reset || G.menu.model_submenu)
        menu_press(KEY_ESCAPE);
    int page = menu_item_page(item);
    if (G.menu.page != MENU_PAGE_MAIN && G.menu.page != page)
        menu_press(KEY_ESCAPE);
    if (G.menu.page == MENU_PAGE_MAIN && page != MENU_PAGE_MAIN) {
        int group = page == MENU_PAGE_MODEL ? MENU_MODEL :
                    page == MENU_PAGE_SCORES ? MENU_SCORES :
                    page == MENU_PAGE_DISPLAY ? MENU_DISPLAY : MENU_RESETS;
        menu_press(KEY_HOME);
        for (int guard = 0; G.menu.selected != group && guard < 12; guard++)
            menu_press(KEY_DOWN);
        menu_press(KEY_ENTER);
    }
    menu_press(KEY_HOME);
    for (int guard = 0; G.menu.selected != item && guard < 12; guard++)
        menu_press(KEY_DOWN);
}

static void menu_close(void)
{
    if (!G.menu.open) return;
    if (G.menu.prompt_edit || G.menu.details_open || G.menu.leaderboard_open ||
        G.menu.confirm_reset || G.menu.model_submenu)
        menu_press(KEY_ESCAPE);
    if (G.menu.open && G.menu.page != MENU_PAGE_MAIN)
        menu_press(KEY_ESCAPE);
    if (G.menu.open && G.started)
        menu_press(KEY_ESCAPE);
}

static void test_menu(void)
{
    char err[256], state[2300];
    key_event ev = {0};
    sub(state, sizeof state, "menu-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "menu game: %s", err);
    G.sc.focus = FOCUS_SHELL;
    ev.action = 1;
    ev.key = KEY_F12;
    ev.mods = MOD_CTRL | MOD_SHIFT;
    game_key(&G, &ev);
    CHECK(G.menu.open, "the game menu chord opens the menu from the shell");
    ev.mods = 0;
    double now = G.now;
    game_update(&G, 1.0);
    CHECK(G.now == now, "menu pauses room movement and hints");

    G.menu.models = calloc(2, sizeof *G.menu.models);
    G.menu.models[0] = (model_option){"first:latest", THINK_SWITCHABLE};
    G.menu.models[1] = (model_option){"last:latest", THINK_UNAVAILABLE};
    G.menu.count = 2;
    CHECK(G.menu.page == MENU_PAGE_MAIN && G.menu.selected == MENU_HARNESS,
          "the menu opens on Harness");
    menu_press(KEY_DOWN);
    CHECK(G.menu.selected == MENU_MODEL, "Model follows Harness");
    menu_press(KEY_DOWN);
    CHECK(G.menu.selected == MENU_RESUME, "Start game follows Model");
    menu_to(MENU_OLLAMA);
    menu_press(KEY_ENTER);
    CHECK(G.menu.open && G.menu.model_submenu == MENU_OLLAMA,
          "Ollama opens its own model list");
    menu_press(KEY_ESCAPE);
    CHECK(G.menu.open && !G.menu.model_submenu && G.menu.page == MENU_PAGE_MODEL &&
          G.menu.selected == MENU_OLLAMA,
          "Escape returns from the model list to Ollama");
    menu_press(KEY_ENTER);
    menu_press(KEY_END);
    menu_press(KEY_ENTER);
    CHECK(!G.menu.open && strcmp(G.ollama_model, "last:latest") == 0,
          "selecting a listed model applies it and closes the menu");
    CHECK(G.sc.focus == FOCUS_SHELL && G.sc.level == 0,
          "model choice preserves room and terminal focus");
    menu_to(MENU_QUIT);
    menu_press(KEY_ENTER);
    CHECK(G.quit, "exit game uses normal quit path");
    game_shutdown(&G);
}

static void test_room_buttons(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "room-buttons");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "room buttons: %s", err);
    rect button = layout_room_button(BUTTON_RESET_LEVEL);
    G.sc.x = button.x + button.w / 2;
    G.sc.y = button.y + button.h / 2;
    menu_press(KEY_ENTER);
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(!G.room_buttons && !G.menu.open, "level buttons start off and ignore input");
    menu_to(MENU_ROOM_BUTTONS);
    menu_press(KEY_ENTER);
    CHECK(G.room_buttons, "the menu enables level buttons");
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err) && G.room_buttons,
          "enabled level buttons survive reopening the save");
    G.sc.x = button.x + button.w / 2;
    G.sc.y = button.y + button.h / 2;
    menu_press(KEY_ENTER);
    CHECK(G.menu.confirm_reset && G.menu.page == MENU_PAGE_RESET && G.menu.selected == MENU_RESET_LEVEL,
          "standing on Reset level opens the reset confirmation");
    menu_close();
    button = layout_room_button(BUTTON_METRICS);
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(G.menu.leaderboard_open && G.menu.score_level == 1,
          "clicking Metrics opens the current room leaderboard");
    menu_close();
    button = layout_room_button(BUTTON_CLEAR_MODEL);
    feed(&G.omp_vt, "old coding session");
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(strcmp(row(&G.omp_vt, 0), "") == 0, "clear model empties the coding pane");

    button = layout_room_button(BUTTON_MODEL);
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(G.menu.open && G.menu.page == MENU_PAGE_MODEL && G.menu.selected == MENU_LOCAL,
          "Model opens the backend selector");
    menu_close();
    button = layout_room_button(BUTTON_HARNESS);
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(G.menu.open && G.menu.page == MENU_PAGE_MAIN && G.menu.selected == MENU_HARNESS,
          "Harness opens its setting");
    menu_close();
    button = layout_room_button(BUTTON_RESET_GAME);
    game_click(&G, button.x + 4, button.y + 4);
    CHECK(G.menu.confirm_reset && G.menu.page == MENU_PAGE_RESET && G.menu.selected == MENU_RESET,
          "Reset game asks for confirmation");
    menu_close();

    char *idle[] = {"/bin/cat", NULL};
    int transfer[2];
    if (pty_spawn(&G.omp, idle, NULL, OMP_COLS, OMP_ROWS) && pipe(transfer) == 0) {
        int master = G.omp.fd;
        G.omp.fd = transfer[1];
        fcntl(transfer[0], F_SETFL, O_NONBLOCK);
        button = layout_room_button(BUTTON_SHELL_LOG);
        game_click(&G, button.x + 4, button.y + 4);
        char input[128];
        const char expected[] = "The player shell transcript is at ~/shell.log.";
        ssize_t n = read(transfer[0], input, sizeof input);
        CHECK(G.sc.focus == FOCUS_OMP && n == (ssize_t)(sizeof expected - 1) &&
              memcmp(input, expected, sizeof expected - 1) == 0,
              "Shell log puts the sandbox path in the active agent's draft");
        close(transfer[0]);
        close(transfer[1]);
        G.omp.fd = master;
    } else CHECK(false, "shell log button test stream initialized");
    game_shutdown(&G);
}
static void test_campaign(void)
{
    char err[256], state[2300], path[2500], locked[SCORE_MODEL_CAP];
    int needed[LEVEL_MAX], needed_count = 0;
    score_entry room = {.level = 1, .id = 9, .measure = {100, 10, -1, -1, -1, -1, -1}};
    score_entry *board = NULL;
    int count = 0;
    long long tokens = 100;
    bool ranked = false;

    sub(state, sizeof state, "campaign-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "campaign game: %s", err);
    for (int i = 0; i < G.levels.count; i++) {
        if (G.levels.items[i].check != CHECK_NONE)
            needed[needed_count++] = G.levels.items[i].number;
    }
    CHECK(needed_count >= 6 && needed[0] == 1, "the campaign covers the playable rooms");
    room.level = needed[0];
    CHECK(!G.campaign, "campaign starts off");
    snprintf(path, sizeof path, "%s/workspace/GOAL.md", state);
    CHECK(file_contains(path, "Hello, World"), "campaign save still publishes ~/GOAL.md");

    menu_to(MENU_CAMPAIGN);
    menu_press(KEY_ENTER);
    CHECK(G.campaign && G.campaign_model[0] && strstr(G.menu.notice, "locked"),
          "campaign locks the current model");
    snprintf(locked, sizeof locked, "%s", G.campaign_model);
    snprintf(room.model, sizeof room.model, "%s", locked);
    snprintf(room.player, sizeof room.player, "Player");

    menu_to(MENU_CODEX);
    menu_press(KEY_ENTER);
    CHECK(!G.use_codex && G.menu.open && strstr(G.menu.notice, "locked"),
          "campaign rejects a different model");

    CHECK(campaign_note(state, &room, needed, needed_count, NULL), "record the first campaign room");
    CHECK(campaign_load(state, &board, &count) && count == 0, "one room does not rank a campaign");
    free(board);
    board = NULL;
    room.measure[SCORE_TOKENS] = 999;
    CHECK(campaign_note(state, &room, needed, needed_count, NULL), "a repeated room keeps the first result");
    room.measure[SCORE_TOKENS] = 10;
    for (int i = 1; i < needed_count; i++) {
        room.level = needed[i];
        tokens += room.measure[SCORE_TOKENS];
        ranked = false;
        CHECK(campaign_note(state, &room, needed, needed_count, &ranked),
              "record campaign room %d", needed[i]);
        CHECK(ranked == (i + 1 == needed_count), "the campaign ranks only when every room is done");
    }
    CHECK(campaign_load(state, &board, &count) && count == 1 && board[0].level == needed_count &&
          board[0].measure[SCORE_TOKENS] == tokens,
          "a finished campaign sums the first result from each room");
    free(board);
    board = NULL;

    CHECK(campaign_restart(state, "Other", "Player", 77), "start a second campaign run");
    for (int i = 0; i < needed_count; i++) {
        room.level = needed[i];
        room.id = 77;
        room.measure[SCORE_TOKENS] = 5;
        snprintf(room.model, sizeof room.model, "Other");
        ranked = false;
        CHECK(campaign_note(state, &room, needed, needed_count, &ranked), "record the shorter campaign");
    }
    CHECK(ranked, "the second campaign is ranked");

    menu_to(MENU_CAMPAIGN_BOARD);
    menu_press(KEY_ENTER);
    CHECK(G.menu.leaderboard_open && G.menu.campaign_board && G.menu.score_count == 2 &&
          G.menu.scores[0].measure[SCORE_TOKENS] == 5 * needed_count && G.menu.scores[0].rank == 1,
          "the campaign board ranks the lower token total first");
    menu_press(KEY_ESCAPE);

    score_entry keep = {.level = 1, .id = 5, .measure = {3, 1, -1, -1, -1, -1, -1},
                        .model = "Z", .player = "one"};
    CHECK(leaderboard_submit(state, &keep), "room score stays available");
    menu_to(MENU_CLEAR_CAMPAIGN);
    menu_press(KEY_ENTER);
    CHECK(G.menu.confirm_reset, "clearing the campaign board asks first");
    menu_press(KEY_ESCAPE);
    CHECK(!G.menu.confirm_reset && campaign_load(state, &board, &count) && count == 2,
          "Escape keeps the campaign board");
    free(board);
    board = NULL;
    count = 0;
    menu_press(KEY_ENTER);
    menu_press(KEY_ENTER);
    CHECK(campaign_load(state, &board, &count) && count == 0, "the campaign board is empty");
    free(board);
    board = NULL;
    count = 0;
    CHECK(leaderboard_load(state, &board, &count) && count == 1 && board[0].id == 5,
          "clearing the campaign board leaves room scores");
    free(board);

    menu_to(MENU_CAMPAIGN);
    menu_press(KEY_ENTER);
    CHECK(!G.campaign && strstr(G.menu.notice, "off"), "campaign can be turned off");
    menu_to(MENU_CODEX);
    menu_press(KEY_ENTER);
    CHECK(G.use_codex && !G.menu.open, "the model can change after campaign is off");

    menu_press(KEY_ESCAPE);
    menu_to(MENU_CAMPAIGN);
    menu_press(KEY_ENTER);
    CHECK(G.campaign && strcmp(G.campaign_model, "Codex") == 0, "turning campaign on locks Codex");
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err) && G.campaign && G.use_codex,
          "a campaign reopens on its locked model");
    snprintf(path, sizeof path, "%s/workspace/GOAL.md", state);
    CHECK(file_contains(path, "Hello, World"), "reopening still writes ~/GOAL.md");
    game_shutdown(&G);
}

static void test_leaderboard(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "leaderboard-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "leaderboard state: %s", err);
    score_entry runs[] = {
        {.level = 1, .id = 1, .measure = {400, 3000, 21, 1000, -1, -1, -1}, .model = "A", .player = "one"},
        {.level = 1, .id = 2, .measure = {200, 4000, 25, 1200, -1, -1, -1}, .model = "A", .player = "two"},
        {.level = 1, .id = 3, .measure = {300, 1000, 32, 800, -1, -1, -1}, .model = "B", .player = "one"},
        {.level = 2, .id = 4, .measure = {100, 100, 5, 500, -1, -1, -1}, .model = "C", .player = "one"},
    };
    for (size_t i = 0; i < sizeof runs / sizeof *runs; i++)
        CHECK(leaderboard_submit(state, &runs[i]), "save leaderboard run %zu", i);
    CHECK(leaderboard_submit(state, &runs[0]), "submission is idempotent");
    menu_to(MENU_LEADERBOARD);
    menu_press(KEY_ENTER);
    CHECK(G.menu.leaderboard_open && G.menu.score_count == 4 && G.menu.scores[0].id == 2 &&
          G.menu.scores[0].rank == 1 && G.menu.scores[1].id == 3,
          "models rank by the best token total for this room");
    menu_press('m');
    CHECK(G.menu.score_metric == SCORE_TIME && G.menu.scores[0].id == 3,
          "time ranking uses the best time for each model");
    menu_press('m');
    menu_press('m');
    CHECK(G.menu.score_metric == SCORE_RAM && G.menu.scores[0].id == 3,
          "peak RAM ranks independently of tokens and time");
    menu_press(KEY_TAB);
    CHECK(!G.menu.model_scores && G.menu.scores[0].id == 2 && G.menu.scores[1].id == 1 &&
          G.menu.scores[1].rank == 2 && G.menu.scores[2].rank == 1,
          "player rankings start at one for each model");
    menu_press(KEY_RIGHT);
    CHECK(G.menu.score_level == 2 && G.menu.scores[3].rank == 1,
          "the leaderboard switches rooms");
    game_shutdown(&G);
}

static void test_renderer_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "renderer-menu");
    draw_set_neuronte(false);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "renderer menu: %s", err);
    CHECK(!game_neuronte_preference(state) && strcmp(draw_renderer(), "system font") == 0,
          "a new save defaults to the system font");
    menu_to(MENU_TOOLTIPS);
    menu_press(KEY_END);
    if (draw_neuronte_available()) {
        CHECK(G.menu.page == MENU_PAGE_DISPLAY && G.menu.selected == MENU_NEURONTE,
              "Neuronte setting is available");
        menu_press(KEY_ENTER);
        CHECK(draw_neuronte_enabled() && game_neuronte_preference(state),
              "enabling switches renderer and saves the setting");
        menu_press(KEY_ENTER);
        CHECK(!draw_neuronte_enabled() && !game_neuronte_preference(state),
              "disabling restores the system font and saves the setting");
    } else {
        CHECK(G.menu.page == MENU_PAGE_DISPLAY && G.menu.selected == MENU_HINTS_FILE,
              "Neuronte setting is hidden when unavailable");
    }
    game_shutdown(&G);
}

static void test_codex_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "codex-menu");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "codex menu: %s", err);
    G.sc.focus = FOCUS_SHELL;
    menu_to(MENU_CODEX);
    menu_press(KEY_ENTER);
    CHECK(G.use_codex && G.act.codex && !G.menu.open && G.sc.focus == FOCUS_SHELL,
          "Codex selection changes the agent and the activity source");
    const char *event = "{\"type\":\"response_item\",\"payload\":"
        "{\"type\":\"function_call\",\"name\":\"exec_command\"}}";
    activity_session_line(&G.act, event, strlen(event));
    CHECK(has_line(&G.act, "tool exec_command"), "Codex tool calls reach the activity screen");

    menu_to(MENU_GROK);
    menu_press(KEY_ENTER);
    CHECK(G.use_grok && G.act.grok && !G.use_codex && !G.menu.open &&
          G.thinking_support == THINK_UNAVAILABLE,
          "Grok is a separate hosted option alongside Codex");
    menu_to(MENU_LOCAL);
    menu_press(KEY_ENTER);
    menu_press(KEY_ENTER);
    CHECK(!G.use_codex && !G.act.codex && !G.use_grok && !G.act.grok &&
          !G.use_claude && !G.act.claude,
          "the bundled model switches back from a hosted agent");
    game_shutdown(&G);
}

static void test_yolo_menu(void)
{
    char err[256], state[2300], path[4096], line[8];
    FILE *f;
    sub(state, sizeof state, "yolo-menu");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "yolo menu: %s", err);
    CHECK(!G.yolo && !G.use_claude, "yolo defaults off and Claude is not selected");
    menu_to(MENU_YOLO);
    menu_press(KEY_ENTER);
    CHECK(G.yolo && G.menu.open, "yolo turns on from the model menu");
    snprintf(path, sizeof path, "%s/yolo", state);
    f = fopen(path, "r");
    CHECK(f && fgets(line, sizeof line, f) && strcmp(line, "on\n") == 0, "yolo preference is saved");
    if (f) fclose(f);
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "yolo reload: %s", err);
    CHECK(G.yolo, "yolo stays on after relaunch");
    menu_to(MENU_CLAUDE);
    menu_press(KEY_ENTER);
    CHECK(G.use_claude && G.act.claude && !G.use_codex && !G.use_grok && !G.act.codex &&
          !G.act.grok && G.yolo && G.thinking_support == THINK_UNAVAILABLE,
          "Claude is a hosted agent and keeps the yolo setting");
    menu_to(MENU_LOCAL);
    menu_press(KEY_ENTER);
    menu_press(KEY_ENTER);
    CHECK(!G.use_claude && !G.act.claude && G.yolo, "leaving Claude clears that agent and keeps yolo");
    game_shutdown(&G);
}

static void test_thinking_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "thinking-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "thinking menu: %s", err);
    CHECK(!G.thinking_enabled, "thinking defaults off at launch");

    menu_press(KEY_ESCAPE);
    G.menu.models = calloc(2, sizeof *G.menu.models);
    G.menu.models[0] = (model_option){"thinking:latest", THINK_SWITCHABLE};
    G.menu.models[1] = (model_option){"plain:latest", THINK_UNAVAILABLE};
    G.menu.count = 2;
    menu_to(MENU_OLLAMA);
    menu_press(KEY_ENTER);
    menu_press(KEY_ENTER);
    CHECK(G.thinking_support == THINK_SWITCHABLE && !G.thinking_enabled,
          "a thinking-capable model starts off");

    menu_to(MENU_THINKING);
    menu_press(KEY_ENTER);
    CHECK(G.thinking_enabled, "thinking can be enabled on a thinking model");
    menu_press(KEY_ENTER);
    CHECK(!G.thinking_enabled, "thinking can be disabled again");
    menu_press(KEY_ENTER);

    menu_to(MENU_OLLAMA);
    menu_press(KEY_ENTER);
    menu_press(KEY_END);
    menu_press(KEY_ENTER);
    CHECK(G.thinking_support == THINK_UNAVAILABLE && !G.thinking_enabled,
          "switching to a different model resets thinking");
    menu_to(MENU_THINKING);
    menu_press(KEY_ENTER);
    CHECK(!G.thinking_enabled, "the switch stays off on a non-thinking model");
    game_shutdown(&G);
}

static void test_model_details_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "model-details");
    layout_set(LAYOUT_MIN_W, LAYOUT_MIN_H);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "model details menu: %s", err);
    menu_to(MENU_MODEL_DETAILS);
    menu_press(KEY_ENTER);
    CHECK(G.menu.open && G.menu.details_open && G.menu.details_tab == DETAILS_LLAMA,
          "model details opens on the active backend");
    menu_press(KEY_LEFT);
    CHECK(G.menu.details_tab == DETAILS_OLLAMA && strstr(G.menu.prompt, "use the matching function tool") &&
          strstr(G.menu.write_schema, "\"required\": [\"path\", \"i\", \"content\"]"),
          "Ollama details load the shipped prompt and write schema");

    sr_canvas top = {0}, end = {0};
    if (sr_canvas_init(&top, CANVAS_W, CANVAS_H) && sr_canvas_init(&end, CANVAS_W, CANVAS_H)) {
        game_render(&G, &top);
        menu_press(KEY_END);
        game_render(&G, &end);
        CHECK(G.menu.details_scroll > 0 &&
              differing(&top, &end, (rect){50, 120, CANVAS_W - 100, CANVAS_H - 210}) > 0,
              "model details scroll to the end of the schema");
        game_scroll(&G, CANVAS_W / 2, CANVAS_H / 2, -12);
        CHECK(G.menu.details_scroll == 0, "wheel scrolls model details");
    } else {
        CHECK(false, "model details test canvases initialized");
    }
    sr_canvas_free(&top);
    sr_canvas_free(&end);
    menu_press(KEY_ENTER);
    CHECK(G.menu.prompt_edit && strstr(G.menu.prompt_draft, "use the matching function tool"),
          "Enter edits the prompt from the workspace");
    key_event clear = {.key = 'u', .mods = MOD_CTRL, .action = 1};
    game_key(&G, &clear);
    const char replacement[] = "Use the write tool.\r\nRead its results.";
    game_paste(&G, (const unsigned char *)replacement, sizeof replacement - 1, true);
    key_event save = {.key = 's', .mods = MOD_CTRL, .action = 1};
    game_key(&G, &save);
    CHECK(!G.menu.prompt_edit && strcmp(G.menu.prompt, "Use the write tool.\nRead its results.") == 0,
          "Ctrl+S saves the multiline prompt and reads it back from the workspace");
    menu_press(KEY_ENTER);
    game_key(&G, &clear);
    menu_press(KEY_ESCAPE);
    CHECK(!G.menu.prompt_edit && strstr(G.menu.prompt, "Read its results."),
          "Escape discards an unsaved prompt edit");
    menu_press(KEY_ESCAPE);
    CHECK(G.menu.open && !G.menu.details_open && G.menu.selected == MENU_MODEL_DETAILS && !G.ollama_model[0],
          "Escape returns to the menu without changing the model");
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "reopen prompt file: %s", err);
    menu_to(MENU_MODEL_DETAILS);
    menu_press(KEY_ENTER);
    menu_press(KEY_LEFT);
    CHECK(strstr(G.menu.prompt, "Read its results."), "opening the game preserves edited system files");
    game_shutdown(&G);
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
}

static void test_tooltips(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "tooltips");
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "tooltip menu: %s", err);
    CHECK(!G.tooltips, "tooltips default off");
    G.sc.x = 200;
    G.sc.y = WALK_Y0;
    G.sc.dir = 1;
    CHECK(scene_near_screen(&G.sc) == SCREEN_OMP, "tooltip test is near a terminal");
    sr_canvas off = {0}, on = {0};
    bool allocated = sr_canvas_init(&off, CANVAS_W, CANVAS_H) &&
                     sr_canvas_init(&on, CANVAS_W, CANVAS_H);
    CHECK(allocated, "tooltip canvases allocated");
    if (allocated) game_render(&G, &off);
    menu_to(MENU_TOOLTIPS);
    menu_press(KEY_ENTER);
    CHECK(G.tooltips && G.menu.open, "menu turns tooltips on");
    menu_close();
    if (allocated) {
        game_render(&G, &on);
        CHECK(differing(&off, &on, (rect){40, WALK_Y0 - 120, 320, 40}) > 0,
              "the toggle shows the yellow character prompt");
    }
    sr_canvas_free(&off);
    sr_canvas_free(&on);
    game_shutdown(&G);

    CHECK(game_init(&G, ".", state, false, err, sizeof err) && G.tooltips,
          "tooltip preference survives relaunch");
    menu_to(MENU_TOOLTIPS);
    menu_press(KEY_ENTER);
    CHECK(!G.tooltips, "menu turns tooltips off again");
    game_shutdown(&G);
}

static void test_compute_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "compute-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "compute menu: %s", err);
    CHECK(G.compute == COMPUTE_AUTO, "compute defaults to Auto");
    menu_to(MENU_COMPUTE);
    menu_press(KEY_ENTER);
    CHECK(G.compute == COMPUTE_CPU, "compute menu selects CPU");
    menu_press(KEY_ENTER);
    CHECK(G.compute == COMPUTE_GPU, "compute menu selects GPU");
    game_shutdown(&G);

    CHECK(game_init(&G, ".", state, false, err, sizeof err), "compute reload: %s", err);
    CHECK(G.compute == COMPUTE_GPU, "compute setting persists");
    menu_to(MENU_CODEX);
    menu_press(KEY_ENTER);
    menu_to(MENU_COMPUTE);
    menu_press(KEY_ENTER);
    CHECK(G.use_codex && G.compute == COMPUTE_GPU, "Codex keeps the local compute setting");
    G.use_codex = false;
    menu_press(KEY_ENTER);
    CHECK(G.compute == COMPUTE_AUTO, "compute menu cycles back to Auto");
    game_shutdown(&G);
}

static void test_start_and_reset_menu(void)
{
    char err[256], state[2300], file[2400], pattern[2500];
    sub(state, sizeof state, "menu-reset-state");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "prepare save: %s", err);
    CHECK(game_save_progress(state, 2), "save level 3");
    snprintf(file, sizeof file, "%s/workspace/keepsake", state);
    write_file(file, "keep me", 7);
    game_shutdown(&G);

    CHECK(game_init(&G, ".", state, true, err, sizeof err), "interactive launch: %s", err);
    CHECK(G.menu.open && !G.started && G.omp.pid == 0 && G.shell.pid == 0,
          "interactive launch waits at menu before starting the terminals");
    menu_press(KEY_ESCAPE);
    CHECK(G.menu.open, "Escape keeps the initial menu open");
    G.spawn = false;  /* Drive the menu without running the terminal children. */
    menu_to(MENU_LOCAL);
    menu_press(KEY_ENTER);
    CHECK(G.menu.model_submenu == MENU_LOCAL && !G.started,
          "llama.cpp opens its own model list before starting");
    menu_press(KEY_ENTER);
    CHECK(G.started && !G.menu.open && G.sc.level == 2,
          "selecting a model starts the game and continues the existing save");

    menu_to(MENU_RESET_LEVEL);
    menu_press(KEY_ENTER);
    CHECK(G.menu.confirm_reset && access(file, F_OK) == 0,
          "reset level asks before changing the workspace");
    menu_press(KEY_ENTER);
    CHECK(G.started && !G.menu.open && G.sc.level == 2 && !G.sc.goal_met &&
          game_load_progress(state, G.levels.count) == 2 && access(file, F_OK) != 0,
          "reset level restarts the current room with a fresh workspace");
    snprintf(pattern, sizeof pattern, "%s/level-reset-3-*/workspace/keepsake", state);
    glob_t level_backup = {0};
    CHECK(glob(pattern, 0, NULL, &level_backup) == 0 && level_backup.gl_pathc == 1,
          "reset level backs up the old workspace");
    globfree(&level_backup);
    write_file(file, "keep me", 7);

    menu_to(MENU_RESET);
    menu_press(KEY_ENTER);
    CHECK(G.menu.confirm_reset && G.sc.level == 2 && access(file, F_OK) == 0,
          "reset asks before changing the save");
    menu_press(KEY_ESCAPE);
    CHECK(G.menu.open && !G.menu.confirm_reset && access(file, F_OK) == 0,
          "Escape cancels reset and keeps the save");

    menu_press(KEY_ENTER);
    menu_press(KEY_ENTER);
    CHECK(G.menu.open && !G.started && G.sc.level == 0 && !G.sc.goal_met,
          "confirmed reset returns to the menu at level 1");
    CHECK(access(file, F_OK) != 0 && game_load_progress(state, G.levels.count) == 0,
          "reset clears the active workspace and persisted level");
    snprintf(pattern, sizeof pattern, "%s/reset-*/workspace/keepsake", state);
    glob_t backup = {0};
    CHECK(glob(pattern, 0, NULL, &backup) == 0 && backup.gl_pathc == 1,
          "reset backs up the old workspace");
    globfree(&backup);

    menu_press(KEY_ENTER);
    CHECK(G.started && !G.menu.open && G.sc.level == 0, "start after reset enters level 1");
    game_shutdown(&G);
}

static void test_harness_menu(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "harness-menu");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "harness menu: %s", err);
    CHECK(!G.use_pi, "OMP is the default harness");
    menu_to(MENU_HARNESS);
    menu_press(KEY_ENTER);
    CHECK(G.use_pi && G.act.pi && !G.use_codex && !G.ollama_model[0],
          "Pi switches the local harness and activity source without changing the model");
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "reopen harness preference: %s", err);
    CHECK(G.use_pi && G.act.pi, "Pi selection survives reopening the save");
    game_shutdown(&G);
}

static void test_pane_sprite_layer(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "pane-sprite");
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "pane sprite game: %s", err);
    sr_canvas before = {0}, after = {0};
    bool ready = sr_canvas_init(&before, CANVAS_W, CANVAS_H) &&
                 sr_canvas_init(&after, CANVAS_W, CANVAS_H);
    CHECK(ready, "pane sprite canvases allocated");
    if (ready) {
        game_render(&G, &before);
        for (int i = SCREEN_OMP; i <= SCREEN_SHELL; i++) {
            rect pane = layout_screen((screen_id)i);
            G.sc.x = pane.x + pane.w / 2;
            G.sc.y = pane.y + pane.h + SCREEN_BEZEL;
            CHECK(scene_walkable(&G.sc, G.sc.x, G.sc.y), "Kilix can stand immediately below pane %d", i);
            game_render(&G, &after);
            rect overlap = {(int)G.sc.x - 35, pane.y + pane.h - 80, 70, 76};
            CHECK(differing(&before, &after, overlap) > 500,
                  "Kilix renders in front of pane %d from the floor", i);
        }
    }
    sr_canvas_free(&before);
    sr_canvas_free(&after);
    game_shutdown(&G);
}


static void test_hints_file(void)
{
    char err[256], state[2300], path[2500], goal[2500];
    sub(state, sizeof state, "hints-file");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "hints file: %s", err);
    snprintf(path, sizeof path, "%s/workspace/HINTS.md", state);
    snprintf(goal, sizeof goal, "%s/workspace/GOAL.md", state);
    CHECK(G.hints_file && file_contains(path, "1. Walk to the agent"),
          "the hint file is on by default");
    menu_to(MENU_HINTS_FILE);
    menu_press(KEY_ENTER);
    CHECK(!G.hints_file && access(path, F_OK) != 0, "turning the hint file off removes ~/HINTS.md");
    write_file(path, "stale hint\n", 11);
    game_enter_level(&G, 0);
    CHECK(access(path, F_OK) != 0 && file_contains(goal, "Hello, World"),
          "entering the room removes a disabled hint file and keeps the goal");
    game_shutdown(&G);
    CHECK(game_init(&G, ".", state, false, err, sizeof err) && !G.hints_file,
          "the hint file stays off after reopening the save");
    snprintf(path, sizeof path, "%s/workspace/HINTS.md", state);
    CHECK(access(path, F_OK) != 0, "reopening does not recreate a disabled hint file");
    menu_to(MENU_HINTS_FILE);
    menu_press(KEY_ENTER);
    CHECK(G.hints_file && file_contains(path, "Walk to the agent"),
          "turning the hint file on writes ~/HINTS.md again");
    game_shutdown(&G);
}

static void test_room_art(void)
{
    room_art art = {0};
    sr_canvas frame = {0};
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    art_load_room(&art, ".");
    bool loaded = art.stone.pixels && art.brick.pixels && art.floor.pixels &&
                  art.door.pixels;
    CHECK(loaded, "all four generated room assets load");
    if (loaded && sr_canvas_init(&frame, CANVAS_W, CANVAS_H)) {
        int x = 24, y = 50;
        art_room(&frame, &art, "stone");
        CHECK(at(&frame, x, y) == (art.stone.pixels[(y * art.stone.h / WALL_H) * art.stone.w + x] & 0xffffff),
              "the stone room uses the stone wall texture");
        CHECK(at(&frame, x, WALL_H + y) == (art.floor.pixels[y * art.floor.w + x] & 0xffffff) &&
              at(&frame, x + art.floor.w, WALL_H + y) == at(&frame, x, WALL_H + y),
              "the slate floor texture repeats");
        art_room(&frame, &art, "brick");
        CHECK(at(&frame, x, y) == (art.brick.pixels[(y * art.brick.h / WALL_H) * art.brick.w + x] & 0xffffff),
              "the brick room uses the brick wall texture");
        art_door(&frame, &art, "brick", 0, 2);
        CHECK(at(&frame, DOOR_X + x, DOOR_Y + y) == (art.door.pixels[y * art.door.w + x] & 0xffffff),
              "the closed door uses the wood sprite");
        uint32_t threshold = at(&frame, DOOR_X + x, DOOR_Y + DOOR_H + 8);
        art_door(&frame, &art, "brick", 0.5f, 2);
        CHECK(at(&frame, DOOR_X + x, DOOR_Y + y) == 0 &&
              at(&frame, DOOR_X + x, DOOR_Y + 20) != 0,
              "the leaf reveals black as it slides");
        art_door(&frame, &art, "brick", 1, 2);
        int colored = 0;
        for (int row = DOOR_Y + 6; row < DOOR_Y + DOOR_H; row++)
            for (int col = DOOR_X; col < DOOR_X + DOOR_W; col++)
                colored += at(&frame, col, row) != 0;
        CHECK(colored == 0, "the open doorway has a solid black interior");
        CHECK(at(&frame, DOOR_X + x, DOOR_Y + DOOR_H + 8) == threshold,
              "the black opening stays inside its frame");
    } else {
        CHECK(false, "room art canvas initialized");
    }
    sr_canvas_free(&frame);
    art_free_room(&art);
}

static void test_pane_focus(void)
{
    char err[256], state[2300];
    sub(state, sizeof state, "pane-focus");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "pane focus: %s", err);
    key_event key = {.key = KEY_RIGHT, .mods = MOD_ALT, .action = 1};
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_SHELL, "Alt+Right focuses shell");
    key.key = KEY_LEFT;
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_SHELL, "Alt-Left stays with the focused shell");
    key = (key_event){.key = KEY_F1, .mods = MOD_CTRL | MOD_SHIFT, .action = 1};
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_OMP, "the game focus chord switches to the agent");
    key.key = KEY_RIGHT;
    key.mods = MOD_ALT | MOD_CTRL;
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_OMP, "modified Alt-arrow stays with the terminal");
    key = (key_event){.key = KEY_F1 + 9, .mods = MOD_CTRL | MOD_SHIFT, .action = 1};
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_ROOM, "the game focus chord returns to the room");
    key = (key_event){.key = KEY_RIGHT, .mods = MOD_ALT, .action = 1};
    game_key(&G, &key);
    key.mods = 0;
    key.action = 2;
    game_key(&G, &key);
    CHECK(G.held_until[1] == 0, "a focus chord repeat cannot move Kilix after Alt is released");
    key.action = 3;
    game_key(&G, &key);
    key = (key_event){.key = KEY_F1, .mods = MOD_CTRL | MOD_SHIFT, .action = 1};
    game_key(&G, &key);
    key = (key_event){.key = KEY_F1 + 9, .mods = MOD_CTRL | MOD_SHIFT, .action = 1};
    game_key(&G, &key);
    key = (key_event){.key = KEY_DOWN, .mods = MOD_ALT, .action = 1};
    game_key(&G, &key);
    CHECK(G.sc.focus == FOCUS_OMP, "Alt+Down restores the last terminal");
    game_shutdown(&G);
}

static void test_goal_and_action_panes(void)
{
    char err[256], state[2300];
    sr_canvas before = {0}, after = {0};
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    sub(state, sizeof state, "goal-action");
    CHECK(game_init(&G, ".", state, false, err, sizeof err), "pane render: %s", err);
    rect goal = layout_screen(SCREEN_GOAL), act = layout_screen(SCREEN_ACT);
    if (sr_canvas_init(&before, CANVAS_W, CANVAS_H) && sr_canvas_init(&after, CANVAS_W, CANVAS_H)) {
        game_render(&G, &before);
        int hints = G.hints_shown;
        game_click(&G, act.x + 10, act.y + 10);
        CHECK(G.hints_shown == hints, "clicking system actions leaves hints alone");
        game_click(&G, goal.x + 10, goal.y + 10);
        game_render(&G, &after);
        CHECK(G.hints_shown == hints + 1 && differing(&before, &after, goal) > 0 &&
              differing(&before, &after, act) == 0, "the next hint changes the goal pane only");

        rect resources = layout_resources();
        snprintf(G.metrics.summary, sizeof G.metrics.summary, "Harness  CPU 125%%  RAM 2.0G");
        snprintf(G.metrics.devices, sizeof G.metrics.devices, "GPU 35%%  VRAM 1.0G  Disk 2M/3M/s");
        game_render(&G, &before);
        rect utilization = {resources.x, resources.y, resources.w, RESOURCE_H / 2};
        rect devices = {resources.x, resources.y + RESOURCE_H / 2, resources.w, RESOURCE_H / 2};
        CHECK(differing(&before, &after, utilization) > 0 &&
              differing(&before, &after, devices) > 0 && differing(&before, &after, act) == 0,
              "both resource rows update without changing system actions");
        G.act.tokens_reported = true;
        G.act.tokens_used = 12345;
        game_render(&G, &after);
        rect tokens = {resources.x + resources.w * 3 / 4, resources.y,
                       resources.w - resources.w * 3 / 4, RESOURCE_H / 2};
        CHECK(differing(&before, &after, tokens) > 0 && differing(&before, &after, devices) == 0,
              "session tokens update on the right side of the resource bar");

        activity_add(&G.act, ACT_TOOL, "tool write hello_world.txt");
        game_render(&G, &before);
        CHECK(differing(&before, &after, act) > 0 && differing(&before, &after, goal) == 0,
              "tool events change the system actions pane only");
    } else {
        CHECK(false, "pane render canvases initialized");
    }
    sr_canvas_free(&before);
    sr_canvas_free(&after);
    game_shutdown(&G);
}

static void test_pane_scroll(void)
{
    char err[256], state[2300];
    sr_canvas live = {0}, scrolled = {0};
    layout_set(LAYOUT_MIN_W, LAYOUT_MIN_H);
    sub(state, sizeof state, "pane-scroll");
    if (!game_init(&G, ".", state, false, err, sizeof err)) {
        CHECK(false, "pane scroll: %s", err);
        return;
    }

    for (int i = 0; i < OMP_ROWS + 20; i++) {
        char line[72];
        snprintf(line, sizeof line, "line %02d\r\n", i);
        neuroterm_feed(&G.omp_vt, line, strlen(line));
        neuroterm_feed(&G.shell_vt, line, strlen(line));
        activity_add(&G.act, ACT_TOOL, "tool call %d", i);
    }
    memset(G.levels.items[0].goal, 'G', sizeof G.levels.items[0].goal - 1);
    rect omp = layout_screen(SCREEN_OMP), shell = layout_screen(SCREEN_SHELL);
    rect act = layout_screen(SCREEN_ACT), goal = layout_screen(SCREEN_GOAL);
    CHECK(G.omp_history.count > 8 && G.shell_history.count > 8,
          "both NeuroTerm panes retain output that scrolls off the screen");
    if (sr_canvas_init(&live, CANVAS_W, CANVAS_H) && sr_canvas_init(&scrolled, CANVAS_W, CANVAS_H)) {
        game_render(&G, &live);
        game_scroll(&G, shell.x + 10, shell.y + 10, -3);
        game_render(&G, &scrolled);
        CHECK(G.shell_history.offset == 3 && G.omp_history.offset == 0 &&
              differing(&live, &scrolled, shell) > 0 && differing(&live, &scrolled, omp) == 0,
              "wheel scrolls the shell without touching the agent");
        game_scroll(&G, omp.x + 10, omp.y + 10, -3);
        VTermScreenCell prior, later;
        history_cell(&G.omp_history, 6, 0, &prior);
        neuroterm_feed(&G.omp_vt, "later\r\n", 7);
        history_cell(&G.omp_history, 6, 0, &later);
        CHECK(G.omp_history.offset == 4 && prior.chars[0] == later.chars[0],
              "new terminal output preserves the previously scrolled view");

        game_scroll(&G, act.x + 10, act.y + 10, -3);
        game_scroll(&G, goal.x + 10, goal.y + 10, 256);
        game_render(&G, &scrolled);
        CHECK(G.action_scroll == 3 && differing(&live, &scrolled, act) > 0,
              "wheel scrolls through system actions");
        CHECK(G.goal_scroll > 0 && differing(&live, &scrolled, goal) > 0,
              "wheel reveals goal and hint text beyond the viewport");
        CHECK(G.sc.focus == FOCUS_ROOM, "wheel scrolling keeps keyboard focus");
        game_scroll(&G, act.x + 10, act.y + 10, 256);
        game_scroll(&G, shell.x + 10, shell.y + 10, 256);
        CHECK(G.action_scroll == 0 && G.shell_history.offset == 0,
              "scrolling down past the end returns to live output");
        game_click(&G, omp.x + 10, omp.y + 10);
        key_event key = {.key = KEY_PAGE_DOWN, .mods = MOD_CTRL | MOD_SHIFT, .action = 1};
        game_key(&G, &key);
        CHECK(G.omp_history.offset == 0, "the game scroll shortcut returns to the live agent");
    } else {
        CHECK(false, "scroll test canvases initialized");
    }

    neuroterm_feed(&G.shell_vt, "\033[?1049h", 8);
    CHECK(G.shell_history.alternate, "NeuroTerm tracks entry to the alternate screen");
    game_scroll(&G, shell.x + 10, shell.y + 10, -3);
    CHECK(G.shell_history.offset == 0, "alternate screen stays separate from shell history");
    neuroterm_feed(&G.shell_vt, "\033[?1049l\033[3J", 12);
    CHECK(!G.shell_history.alternate && G.shell_history.count == 0,
          "the terminal can clear its scrollback");
    sr_canvas_free(&live);
    sr_canvas_free(&scrolled);
    game_shutdown(&G);
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
}

static void test_pane_clipboard(void)
{
    char err[256], state[2300], bytes[128];
    key_event copy = {.key = 'c', .mods = MOD_CTRL | MOD_ALT, .action = 1};
    key_event paste = {.key = 'v', .mods = MOD_CTRL | MOD_ALT, .action = 1};
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    sub(state, sizeof state, "pane-clipboard");
    if (!game_init(&G, ".", state, false, err, sizeof err)) {
        CHECK(false, "pane clipboard: %s", err);
        return;
    }
    rect omp = layout_text_bounds(SCREEN_OMP), act = layout_text_bounds(SCREEN_ACT);
    rect goal = layout_text_bounds(SCREEN_GOAL), shell = layout_text_bounds(SCREEN_SHELL);
    activity_add(&G.act, ACT_TOOL, "tool result");
    game_pointer(&G, act.x, act.y, POINTER_DOWN);
    game_pointer(&G, act.x + 3 * CELL_W, act.y, POINTER_UP);
    CHECK(strcmp(G.primary, "tool") == 0 && G.clipboard[0] == 0,
          "highlighting sets the separate primary selection without a copy key");
    game_key(&G, &copy);
    CHECK(strcmp(G.clipboard, "tool") == 0, "system action text can be copied");

    int hints = G.hints_shown;
    game_pointer(&G, goal.x, goal.y, POINTER_DOWN);
    game_pointer(&G, goal.x + 2 * CELL_W, goal.y, POINTER_UP);
    game_key(&G, &copy);
    CHECK(strcmp(G.clipboard, "Get") == 0 && G.hints_shown == hints,
          "selection can copy the goal without advancing hints");

    const char *output = "caf\xc3\xa9\r\n\xe7\x8c\xab!";
    neuroterm_feed(&G.omp_vt, output, strlen(output));
    game_pointer(&G, omp.x + 2 * CELL_W, omp.y + CELL_H, POINTER_DOWN);
    game_pointer(&G, omp.x, omp.y, POINTER_DRAG);
    game_pointer(&G, omp.x, omp.y, POINTER_UP);
    game_key(&G, &copy);
    CHECK(strcmp(G.clipboard, "caf\xc3\xa9\n\xe7\x8c\xab!") == 0 &&
          G.sc.focus == FOCUS_ROOM, "reverse drag copies multiline Unicode terminal text");

    int transfer[2];
    char *idle[] = {"sleep", "60", NULL};
    if (pty_spawn(&G.shell, idle, NULL, SHELL_COLS, SHELL_ROWS) && pipe(transfer) == 0) {
        int master = G.shell.fd;
        G.shell.fd = transfer[1];
        fcntl(transfer[0], F_SETFL, O_NONBLOCK);
        game_pointer(&G, omp.x, omp.y, POINTER_DOWN);
        game_pointer(&G, omp.x + 3 * CELL_W, omp.y, POINTER_UP);
        neuroterm_feed(&G.shell_vt, "\033[?2004h", 8);
        game_pointer(&G, goal.x, goal.y, POINTER_MIDDLE);
        CHECK(G.sc.focus == FOCUS_ROOM && G.hints_shown == hints,
              "middle-click ignores the read-only goal pane");
        game_pointer(&G, shell.x, shell.y, POINTER_MIDDLE);
        const char *expected = "\033[200~caf\xc3\xa9\033[201~";
        ssize_t n = read(transfer[0], bytes, sizeof bytes);
        CHECK(G.sc.focus == FOCUS_SHELL && n == (ssize_t)strlen(expected) &&
              memcmp(bytes, expected, (size_t)n) == 0,
              "middle-click pastes the highlight into the terminal under the pointer");

        game_key(&G, &paste);
        expected = "\033[200~caf\xc3\xa9\r\xe7\x8c\xab!\033[201~";
        n = read(transfer[0], bytes, sizeof bytes);
        CHECK(n == (ssize_t)strlen(expected) && memcmp(bytes, expected, (size_t)n) == 0,
              "clipboard pastes into the focused shell with bracketed paste");

        game_paste(&G, (const unsigned char *)"a\r", 2, false);
        CHECK(read(transfer[0], bytes, sizeof bytes) < 0 && errno == EAGAIN,
              "outer paste chunks wait for the final event");
        game_paste(&G, (const unsigned char *)"\nb\033", 3, true);
        expected = "\033[200~a\rb\033[201~";
        n = read(transfer[0], bytes, sizeof bytes);
        CHECK(n == (ssize_t)strlen(expected) && memcmp(bytes, expected, (size_t)n) == 0,
              "native paste joins chunks and strips embedded controls");
        const key_event terminal_keys[] = {
            {.key = KEY_ESCAPE, .action = 1},
            {.key = KEY_LEFT, .mods = MOD_ALT, .action = 1},
            {.key = KEY_F1, .action = 1},
            {.key = KEY_F1 + 9, .action = 1},
            {.key = ']', .mods = MOD_CTRL, .action = 1},
            {.key = KEY_PAGE_UP, .mods = MOD_SHIFT, .action = 1},
        };
        for (size_t i = 0; i < sizeof terminal_keys / sizeof *terminal_keys; i++) {
            char encoded[128];
            size_t len = neuroterm_key(&G.shell_vt, &terminal_keys[i], encoded, sizeof encoded);
            game_key(&G, &terminal_keys[i]);
            n = read(transfer[0], bytes, sizeof bytes);
            CHECK(len > 0 && n == (ssize_t)len && memcmp(bytes, encoded, len) == 0 &&
                  !G.menu.open && G.sc.focus == FOCUS_SHELL,
                  "terminal key %zu reaches the shell unchanged", i);
        }
        close(transfer[0]);
        close(transfer[1]);
        G.shell.fd = master;
    } else {
        CHECK(false, "clipboard transfer test stream initialized");
    }
    game_shutdown(&G);
}

int main(void)
{
    const char *dir = getenv("PUZZLLM_TEST_DIR");
    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "set PUZZLLM_TEST_DIR to an empty scratch folder\n");
        return 2;
    }
    snprintf(scratch, sizeof scratch, "%s", dir);
    test_menu();
    test_pane_focus();
    test_goal_and_action_panes();
    test_pane_scroll();
    test_pane_clipboard();
    test_thinking_menu();
    test_codex_menu();
    test_yolo_menu();
    test_leaderboard();
    test_campaign();
    test_room_buttons();
    test_renderer_menu();
    test_room_art();
    test_pane_sprite_layer();
    test_compute_menu();
    test_tooltips();
    test_hints_file();
    test_model_details_menu();
    test_start_and_reset_menu();
    test_harness_menu();
    test_neuroterm();
    test_keys();
    test_levels();
    test_script_and_stamp();
    test_phrase_checks();
    test_strawberry();
    test_million_ones();
    test_json();
    test_activity();
    test_scene();
    test_kilix_art();
    test_ticks();
    test_layout();
    test_draw();
    test_omp_glyphs();
    test_damage();
    layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    draw_force_raster(true);
    test_render_and_flow();
    draw_force_raster(false);
    if (draw_init()) test_render_and_flow();   /* again, drawn by the nets */
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
