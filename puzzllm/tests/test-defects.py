#!/usr/bin/env python3
"""test-defects.py SCRATCH - planted-defect controls.

Each defect is one exact edit to a copy of the game. The edit must apply
exactly once (so a stale defect cannot pass silently), and the named test
target must then FAIL. A defect the tests do not notice is a hole in the
tests, and this script fails.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old, new, make target[, "neuronte" when it needs that build])
DEFECTS = [
    ("the chest uses the closed sprite after opening",
     "src/art.c", "const chest_sprite *sprite = open ? &art->open : &art->closed;",
     "const chest_sprite *sprite = &art->closed;", "test-unit"),
    ("the open chest hides the key",
     "src/art.c", "if (open) {\n        const chest_sprite *key",
     "if (false) {\n        const chest_sprite *key", "test-unit"),
    ("escape cannot open the game menu",
     "src/game.c", "            else if (!g->menu.open) open_menu(g);", "", "test-unit"),
    ("interactive launch skips the menu",
     "src/game.c", "    if (spawn) open_menu(g);", "", "test-unit"),
    ("menu reset skips confirmation",
     "src/game.c", "if (menu->selected == MENU_RESET) menu->confirm_reset = true;",
     "if (menu->selected == MENU_RESET) reset_from_menu(g);", "test-unit"),
    ("menu reset keeps the current room",
     "src/game.c", "    game_enter_level(g, 0);", "", "test-unit"),
    ("selecting an Ollama model does not apply it",
     "src/game.c", '                snprintf(g->ollama_model, sizeof g->ollama_model, "%s", name);',
     "", "test-unit"),
    ("cloud entries are listed as local models",
     "libexec/puzzllm-ollama",
     '            and not entry.get("remote_host") and not entry.get("remote_model")\n',
     "", "test-sandbox"),
    ("the Ollama bridge permits another model",
     "libexec/puzzllm-ollama", ' or request.get("model") != self.server.model',
     "", "test-sandbox"),
    ("the Ollama bridge accepts external media",
     "libexec/puzzllm-ollama", 'not text_content(message.get("content"))',
     "False", "test-sandbox"),
    ("the Ollama bridge permits management paths",
     "libexec/puzzllm-ollama", "        if self.path != CHAT:", "        if False:", "test-sandbox"),
    # --- sandbox ---
    ("sandbox shares the network",
     "libexec/puzzllm-sandbox",
     "--unshare-all --die-with-parent",
     "--unshare-all --share-net --die-with-parent", "test-sandbox"),
    ("sandbox binds the host root read-write at /mnt/host",
     "libexec/puzzllm-sandbox",
     "    --ro-bind /usr /usr\n",
     "    --ro-bind /usr /usr --bind / /mnt/host\n", "test-sandbox"),
    ("sandbox root left writable",
     "libexec/puzzllm-sandbox",
     'exec bwrap "${args[@]}" --remount-ro / -- "$@"',
     'exec bwrap "${args[@]}" -- "$@"', "test-sandbox"),
    ("sandbox shows the host home",
     "libexec/puzzllm-sandbox",
     "    --tmpfs /tmp --tmpfs /home --tmpfs \"$box_home\"\n",
     "    --tmpfs /tmp --tmpfs /home --tmpfs \"$box_home\" --ro-bind \"$HOME\" \"$HOME\"\n",
     "test-sandbox"),
    ("shell gets the workspace read-only",
     "libexec/puzzllm-sandbox",
     '    args+=(--bind "$state/workspace" "$box_home/workspace"\n           --chdir',
     '    args+=(--ro-bind "$state/workspace" "$box_home/workspace"\n           --chdir',
     "test-sandbox"),
    ("omp pointed at the wrong port",
     "libexec/puzzllm-sandbox",
     "    baseUrl: http://127.0.0.1:$port/v1",
     "    baseUrl: http://127.0.0.1:1$port/v1", "test-sandbox"),
    # --- game ---
    ("level check follows symlinks",
     "src/levels.c", "    if (lstat(path, &st) != 0) {", "    if (stat(path, &st) != 0) {",
     "test-unit"),
    ("an exact size accepts anything bigger",
     "src/levels.c", "        if (q->kind == REQ_BYTES && st.st_size != q->number) {",
     "        if (q->kind == REQ_BYTES && st.st_size < q->number) {", "test-unit"),
    ("only never looks at the last byte",
     "src/levels.c", "            for (long j = 0; j < n; j++) {",
     "            for (long j = 0; j + 1 < n; j++) {", "test-unit"),
    ("a level's size does not raise the read limit",
     "src/levels.c", "    cap = file_cap(lv);", "    cap = CHECK_FILE_CAP;", "test-unit"),
    ("contains-any-ci only tries the first alternative",
     "src/levels.c", "            for (int j = 0; j < k && hit < 0; j++)",
     "            for (int j = 0; j < 1 && hit < 0; j++)", "test-unit"),
    ("lacks-ci is never checked",
     "src/levels.c", "            if (q->kind == REQ_LACKS_CI && hit >= 0) {",
     "            if (q->kind == REQ_LACKS_CI && hit >= 0 && 0) {", "test-unit"),
    ("contains-ci is case-sensitive",
     "src/levels.c",
     "            if (ci) { a = (unsigned char)tolower(a); b = (unsigned char)tolower(b); }",
     "            (void)ci;", "test-unit"),
    ("NeuroTerm does not enable UTF-8",
     "src/neuroterm.c", "vterm_set_utf8(t->term, 1);", "vterm_set_utf8(t->term, 0);", "test-unit"),
    ("NeuroTerm cannot enter the alternate screen",
     "src/neuroterm.c", "vterm_screen_enable_altscreen(t->screen, 1);",
     "vterm_screen_enable_altscreen(t->screen, 0);", "test-unit"),
    ("NeuroTerm misreads the optional RGB colour-space slot",
     "third_party/libvterm/src/pen.c", "    first = 3;", "    first = 2;", "test-unit"),
    ("NeuroTerm ignores terminal application cursor mode",
     "src/neuroterm.c", "vterm_keyboard_key(t->term, navigation[event->key - KEY_LEFT], mods);",
     "return keys_encode(event, false, buf, cap);", "test-unit"),
    ("door passable while shut",
     "src/scene.c", "    return s->door >= 0.95f;", "    return s->door >= 0.0f;",
     "test-unit"),
    ("application cursor keys ignored",
     "src/keys.c",
     "        n = snprintf(b, sizeof b, \"\\033%c%c\", app ? 'O' : '[', letter);",
     "        n = snprintf(b, sizeof b, \"\\033%c%c\", app ? '[' : '[', letter);",
     "test-unit"),
    ("progress not saved",
     "src/game.c", "!game_save_progress(g->state, previous + 1)", "false", "test-unit"),
    ("shell is placed above the activity panel",
     "src/layout.c", "    n.act_y = top;", "    n.act_y = top + act_h + 22;", "test-unit"),
    ("transition leaves files in the workspace",
     "src/game.c", "!archive_workspace(g->workspace, previous + 1)", "false", "test-unit"),
    ("room entry retains the old feed",
     "src/activity.c", "    a->head = a->count = 0;\n", "", "test-unit"),
    ("any same-length key unlocks the chest",
     "src/game.c", " && memcmp(expected, actual, sizeof actual) == 0", "", "test-cli"),
    ("the chest archive has no password",
     "src/game.c", ' "-P", passwords[random[16] % 5],', "", "test-cli"),
    ("reset forgets to clear progress",
     "src/game.c", 'snprintf(path, sizeof path, "%s/progress", state);',
     'snprintf(path, sizeof path, "%s/unused-progress", state);', "test-cli"),
    ("surrogate pairs decoded wrong",
     "src/json.c",
     "cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);",
     "cp = 0x10000 + ((cp - 0xd800) << 9) + (lo - 0xdc00);", "test-unit"),
    ("goal not re-checked while playing",
     "src/game.c", "        game_poll_goal(g);\n    }", "    }", "test-unit"),
    # --- Zell's character and pace, the frame clock, the pane-size room ---
    ("walking pace doubled",
     "src/scene.h", "#define SCENE_WALK_SPEED 3.5f", "#define SCENE_WALK_SPEED 7.0f",
     "test-unit"),
    ("one tick per frame, whatever the frame rate",
     "src/game.c", "    g->tick_debt += dt;\n", "    g->tick_debt += 1.0 / SCENE_TICK_HZ;\n",
     "test-unit"),
    ("walk frames never advance",
     "src/kilix_art.c", "dir * 4 + ((step_ticks - 1) / 7) % 4;", "dir * 4;", "test-unit"),
    ("room ignores the pane's size",
     "src/layout.c", "    w = clamp(w, LAYOUT_MIN_W, LAYOUT_MAX_W);\n",
     "    w = LAYOUT_DEFAULT_W;\n", "test-unit"),
    ("terminals keep their size when the room grows",
     "src/game.c",
     "    if (neuroterm_resize(&g->omp_vt, OMP_COLS, OMP_ROWS)) pty_resize(&g->omp, OMP_COLS, OMP_ROWS);\n",
     "", "test-unit"),
    ("cached room kept across a resize",
     "src/render.c", "    if (bg.px == NULL || bg.w != c->w || bg.h != c->h ||",
     "    if (bg.px == NULL ||", "test-unit"),
    ("a change at the ragged right edge is never sent",
     "src/damage.c", "        for (int t = 0; t <= tiles; t++) {",
     "        for (int t = 0; t < tiles; t++) {", "test-unit"),
    ("sent regions not remembered",
     "src/damage.c", "                copy_rect(prev, cur, w, r);\n", "", "test-unit"),
    # Neuronte only: skipped when this build has no Neuronte
    ("Neuronte text drawn by the 8x16 font",
     "src/draw.c",
     "        neuronte_draw_utf8(font, c->px, c->w, c->w, c->h, px, py, CW, CH, s, len,\n"
     "                           nn_color(rgb, alpha));\n        return;\n",
     "", "test-unit", "neuronte"),
    ("octant cells take the wrong sub-cell",
     "src/draw.c", "static const unsigned char number[8] = {0, 1, 2, 3, 6, 7, 4, 5};",
     "static const unsigned char number[8] = {0, 1, 2, 3, 4, 5, 6, 7};", "test-unit", "neuronte"),
]


def copy_tree(dst):
    """The game directory without test scratch; objects keep their times so
    only the edited file is rebuilt. The SDK link becomes absolute."""
    def ignore(path, names):
        if os.path.abspath(path) == os.path.join(HERE, "build"):
            return [n for n in names if n not in ("obj", "puzzllm", "test-puzzllm")]
        return []
    shutil.copytree(HERE, dst, symlinks=True, ignore=ignore)
    sdk = os.path.realpath(os.path.join(HERE, "third_party", "kilix-game-sdk"))
    os.remove(os.path.join(dst, "third_party", "kilix-game-sdk"))
    os.symlink(sdk, os.path.join(dst, "third_party", "kilix-game-sdk"))


def main():
    scratch = sys.argv[1]
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    kit = os.path.join(HERE, "build", "kilix-game-kit")
    holes = 0
    ident = os.path.join(HERE, "build", "render.ident")
    neuronte = os.path.exists(ident) and open(ident).read().startswith("neuronte")
    skipped = 0
    for i, defect in enumerate(DEFECTS, 1):
        name, path, old, new, target = defect[:5]
        if len(defect) > 5 and defect[5] == "neuronte" and not neuronte:
            print("skip  defect %d (%s): this build has no Neuronte" % (i, name))
            skipped += 1
            continue
        dst = os.path.join(scratch, "d%02d" % i)
        copy_tree(dst)
        f = os.path.join(dst, path)
        text = open(f).read()
        if text.count(old) != 1:
            print("BROKEN defect %d (%s): edit matches %d times in %s"
                  % (i, name, text.count(old), path))
            holes += 1
            continue
        open(f, "w").write(text.replace(old, new))
        log = os.path.join(scratch, "d%02d.log" % i)
        with open(log, "w") as out:
            rc = subprocess.call(
                ["make", "-C", dst, "KILIX_GAME_KIT_BUILD_DIR=" + kit, target],
                stdout=out, stderr=subprocess.STDOUT)
        if rc == 0:
            print("HOLE  defect %d (%s): %s still passes" % (i, name, target))
            holes += 1
        else:
            print("red   defect %d (%s): %s fails" % (i, name, target))
        shutil.rmtree(dst)
    print("test-defects: %d defects, %d not caught, %d skipped"
          % (len(DEFECTS) - skipped, holes, skipped))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
