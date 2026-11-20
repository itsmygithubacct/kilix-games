#!/bin/sh
# test-cli.sh BIN SCRATCH - the command line: version, level check, render.
set -u
bin=$1
scratch=$2
here=$(cd "$(dirname "$0")/.." && pwd)
fails=0
pass() { printf 'ok   %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }

rm -rf "$scratch"
mkdir -p "$scratch"
export PUZZLLM_ROOT="$here" PUZZLLM_STATE="$scratch/state"

out=$("$bin" --version) && [ "$out" = "puzzllm 0.2.0" ] && pass "--version" || fail "--version: $out"
out=$("$here/bin/puzzllm" --version) && [ "$out" = "puzzllm 0.2.0" ] &&
    pass "bin/puzzllm launcher" || fail "launcher: $out"

"$bin" --check-level 1 > "$scratch/out"; rc=$?
[ $rc -eq 1 ] && grep -q 'not met: hello_world.txt does not exist yet' "$scratch/out" &&
    pass "level 1 not met on a fresh state" || fail "fresh check rc=$rc $(cat "$scratch/out")"
printf 'Hello, World!\n' > "$scratch/state/workspace/hello_world.txt"
"$bin" --check-level 1 > "$scratch/out"; rc=$?
[ $rc -eq 0 ] && grep -q 'level 1: met' "$scratch/out" && pass "level 1 met" ||
    fail "met check rc=$rc $(cat "$scratch/out")"
head -c 1000000 /dev/zero | tr '\0' 1 > "$scratch/state/workspace/1.txt"
"$bin" --check-level 3 > "$scratch/out"; rc=$?
[ $rc -eq 0 ] && grep -q 'level 3: met' "$scratch/out" && pass "level 3 met by a million ones" ||
    fail "level 3 rc=$rc $(cat "$scratch/out")"
echo >> "$scratch/state/workspace/1.txt"
"$bin" --check-level 3 > "$scratch/out"; rc=$?
[ $rc -eq 1 ] && grep -q 'one too many: often a line ending' "$scratch/out" &&
    pass "level 3 not met with a line ending" || fail "level 3 newline rc=$rc $(cat "$scratch/out")"
printf '2\n' > "$scratch/state/workspace/strawberry.txt"
"$bin" --check-level 2 > "$scratch/out"; rc=$?
[ $rc -eq 1 ] && grep -q 'must contain only the correct answer' "$scratch/out" &&
    pass "level 2 refuses the wrong count" || fail "level 2 wrong rc=$rc $(cat "$scratch/out")"
printf '3\n' > "$scratch/state/workspace/strawberry.txt"
"$bin" --check-level 2 > "$scratch/out"; rc=$?
[ $rc -eq 0 ] && grep -q 'level 2: met' "$scratch/out" && pass "level 2 accepts the count" ||
    fail "level 2 correct rc=$rc $(cat "$scratch/out")"
"$bin" --check-level 9 2>/dev/null; rc=$?
[ $rc -eq 2 ] && pass "unknown level is an error" || fail "level 9 rc=$rc"
"$bin" --bogus 2>/dev/null; rc=$?
[ $rc -eq 2 ] && pass "bad argument is an error" || fail "bad argument rc=$rc"
"$bin" < /dev/null > /dev/null 2>&1; rc=$?
[ $rc -eq 1 ] && pass "refuses to play without a terminal" || fail "no-tty rc=$rc"

mkdir -p "$scratch/render"
"$bin" --render "$scratch/render" > /dev/null; rc=$?
n=0
for f in "$scratch"/render/*.ppm; do
    [ "$(head -c 15 "$f")" = "$(printf 'P6\n1280 800\n255')" ] && n=$((n + 1))
done
[ $rc -eq 0 ] && [ $n -eq 3 ] && pass "--render writes three 1280x800 frames" ||
    fail "render rc=$rc frames=$n"

# the frame timer: a busy frame at the size given, and which renderer drew it
"$bin" --bench 1366x768 > "$scratch/bench"; rc=$?
[ $rc -eq 0 ] && grep -q '^renderer: ' "$scratch/bench" &&
    grep -q '^frame 1366x768: .* ms/frame average' "$scratch/bench" &&
    pass "--bench times a 1366x768 frame: $(grep '^frame' "$scratch/bench" | cut -d: -f2)" ||
    fail "--bench rc=$rc $(cat "$scratch/bench")"
"$bin" --bench big 2>/dev/null; rc=$?
[ $rc -eq 2 ] && pass "--bench with a bad size is an error" || fail "--bench big rc=$rc"
out=$(PUZZLLM_RENDER=raster "$bin" --renderer) && [ "$out" = raster ] &&
    pass "PUZZLLM_RENDER=raster turns Neuronte off" || fail "PUZZLLM_RENDER=raster: $out"
if grep -q '^neuronte' "$here/build/render.ident" 2>/dev/null; then
    out=$("$bin" --renderer)
    case $out in neuronte*) pass "a Neuronte build draws with Neuronte" ;;
        *) fail "--renderer in a Neuronte build: $out" ;; esac
    out=$(PUZZLLM_NEURONTE_WEIGHTS="$scratch/none.nrt" "$bin" --renderer)
    case $out in "raster (Neuronte weights did not open: $scratch/none.nrt)")
        pass "missing weights fall back to raster, and say so" ;;
        *) fail "missing weights: $out" ;; esac
fi

# Resume the chest room, then solve it through a real encrypted archive.
printf 'level 5\n' > "$PUZZLLM_STATE/progress"
workspace=$PUZZLLM_STATE/workspace
"$bin" --check-level 5 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ -f "$workspace/chest.zip" ] &&
    pass "chest room creates a ZIP and awaits the key" || fail "chest setup rc=$rc"
printf '%032d\n' 0 > "$workspace/key.txt"
"$bin" --check-level 5 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && pass "wrong key leaves the door shut" || fail "wrong key rc=$rc"
unzip -t -P wrong "$workspace/chest.zip" >/dev/null 2>&1; rc=$?
[ "$rc" -ne 0 ] && pass "chest rejects a wrong password" || fail "chest is unencrypted"
hits=0
for candidate in zell qwen puzl kilx pleb; do
    if unzip -P "$candidate" -p "$workspace/chest.zip" key.txt > "$scratch/key" 2>/dev/null; then
        cp "$scratch/key" "$workspace/key.txt"
        hits=$((hits + 1))
    fi
done
"$bin" --check-level 5 > "$scratch/out"; rc=$?
[ "$hits" -eq 1 ] && [ "$rc" -eq 0 ] &&
    pass "exactly one candidate extracts a working key" || fail "chest matches=$hits goal=$rc"
"$bin" --reset-progress > "$scratch/out" &&
    [ ! -e "$PUZZLLM_STATE/progress" ] && [ ! -e "$workspace/key.txt" ] &&
    [ ! -e "$PUZZLLM_STATE/chest-key" ] &&
    find "$PUZZLLM_STATE" -path '*/reset-*/workspace/chest.zip' | grep -q . &&
    pass "reset clears progress and keeps a workspace backup" || fail "reset"

# Restart with a saved transition and a partially populated archive.
printf 'level 1\n' > "$PUZZLLM_STATE/transition"
mkdir "$workspace/level_1"
printf 'old\n' > "$workspace/level_1/hello_world.txt"
printf 'Hello, World!\n' > "$workspace/hello_world.txt"
printf 'shell transcript\n' > "$workspace/shell.log"
"$bin" --check-level 1 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q '^level 2$' "$PUZZLLM_STATE/progress" &&
    [ ! -e "$PUZZLLM_STATE/transition" ] &&
    grep -q '^shell transcript$' "$workspace/shell.log" &&
    grep -q '^old$' "$workspace/level_1/hello_world.txt" &&
    grep -q '^Hello, World!$' "$workspace/level_1/hello_world.txt.1" &&
    pass "interrupted transition resumes without overwriting archived files" ||
    fail "transition recovery rc=$rc"

# Room 4 ships the actual broken Bash script and a folder with tricky names.
printf 'level 4\n' > "$PUZZLLM_STATE/progress"
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q 'did not print the correct count for sample files' "$scratch/out" &&
    [ -f "$workspace/sample files/beta notes.txt" ] &&
    [ -f "$workspace/sample files/.hidden" ] && [ -f "$workspace/sample files/archive/old.txt" ] &&
    pass "room 4 ships the folder and rejects the broken script" || fail "room 4 setup rc=$rc"
printf 'echo 3\n' > "$workspace/count_files.sh"
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q 'must also work with other folder paths' "$scratch/out" &&
    pass "a hard-coded count fails the extra folder checks" || fail "room 4 constant rc=$rc"
cat > "$workspace/count_files.sh" <<'EOF'
folder=${1:-sample files}
find "$folder" -maxdepth 1 -mindepth 1 -type f -printf . | wc -c
EOF
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -q 'level 4: met' "$scratch/out" &&
    grep -q '^find ' "$workspace/count_files.sh" &&
    pass "a general fix opens room 4 and survives reopening the save" || fail "room 4 fix rc=$rc"
printf 'An extra file for the same script.\n' > "$workspace/sample files/new file.txt"
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && [ -f "$workspace/sample files/new file.txt" ] &&
    pass "the grader uses the actual shipped directory in the workspace" || fail "room 4 changed folder rc=$rc"

# The completed script room leads to the final playable room.
printf 'Keep this instruction across rooms.\n' > "$workspace/.system/llama-prompt.txt"
printf 'level 4\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 5 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -qx 'level 5' "$PUZZLLM_STATE/progress" &&
    [ -f "$workspace/level_4/count_files.sh" ] && [ -f "$workspace/chest.zip" ] &&
    pass "room 4 leads to the password-protected archive in room 5" ||
    fail "room 4 to room 5 transition rc=$rc"
grep -qx 'Keep this instruction across rooms.' "$workspace/.system/llama-prompt.txt" &&
    pass "prompt settings stay in the sandbox home across rooms" || fail "prompt settings were archived"

# A crash after archiving the departing room resumes the restore phase.
mkdir -p "$workspace/level_5"
mv "$workspace/chest.zip" "$workspace/level_5/chest.zip"
chest_sum=$(sha256sum < "$workspace/level_5/chest.zip")
printf 'restore-back 5\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'level 4' "$PUZZLLM_STATE/progress" &&
    [ -f "$workspace/count_files.sh" ] && [ ! -e "$PUZZLLM_STATE/transition" ] &&
    pass "interrupted backtrack restores room 4 work and progress" || fail "backtrack recovery rc=$rc"
printf 'level 4\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 5 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ "$(sha256sum < "$workspace/chest.zip")" = "$chest_sum" ] &&
    pass "returning to room 5 restores the original encrypted archive" || fail "room 5 restore rc=$rc"

# Room 6 uses the latest complete failure in the player shell transcript.
printf 'level 6\n' > "$PUZZLLM_STATE/progress"
"$bin" --check-level 6 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ -x "$workspace/run_check.sh" ] &&
    pass "room 6 installs the shell check" || fail "room 6 setup rc=$rc"
run_report() {
    "$here/libexec/puzzllm-sandbox" shell-exec -- ./run_check.sh >> "$workspace/shell.log" &&
        sed -n 's/^PuzzLLM check failed: //p' "$workspace/shell.log" | tail -1
}
first=$(run_report)
printf '%s\n' "$first" > "$workspace/report.txt"
"$bin" --check-level 6 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && pass "room 6 accepts the latest shell report" || fail "room 6 report rc=$rc"
second=$(run_report)
"$bin" --check-level 6 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ "$first" != "$second" ] &&
    pass "room 6 rejects a stale report after another check" || fail "room 6 stale report rc=$rc"
printf '%s\n' "$second" > "$workspace/report.txt"
"$bin" --check-level 6 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && pass "room 6 accepts the updated report" || fail "room 6 updated report rc=$rc"

# a broken level file is reported with its line
mkdir -p "$scratch/root/levels"
printf 'level: 1\ntitle: t\ngoal: g\ncheck: file ../escape\n' > "$scratch/root/levels/levels.txt"
PUZZLLM_ROOT="$scratch/root" "$bin" --check-level 1 2> "$scratch/err"; rc=$?
[ $rc -eq 2 ] && grep -q 'line 4: check file must be a plain' "$scratch/err" &&
    pass "bad level file reported" || fail "bad level file rc=$rc $(cat "$scratch/err")"

echo "test-cli: $fails failed"
[ $fails -eq 0 ]
