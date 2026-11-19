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
printf 'level 4\n' > "$PUZZLLM_STATE/progress"
workspace=$PUZZLLM_STATE/workspace
"$bin" --check-level 4 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ -f "$workspace/chest.zip" ] &&
    pass "chest room creates a ZIP and awaits the key" || fail "chest setup rc=$rc"
printf '%032d\n' 0 > "$workspace/key.txt"
"$bin" --check-level 4 > "$scratch/out"; rc=$?
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
"$bin" --check-level 4 > "$scratch/out"; rc=$?
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
"$bin" --check-level 1 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q '^level 2$' "$PUZZLLM_STATE/progress" &&
    [ ! -e "$PUZZLLM_STATE/transition" ] &&
    grep -q '^old$' "$workspace/level_1/hello_world.txt" &&
    grep -q '^Hello, World!$' "$workspace/level_1/hello_world.txt.1" &&
    pass "interrupted transition resumes without overwriting archived files" ||
    fail "transition recovery rc=$rc"

# a broken level file is reported with its line
mkdir -p "$scratch/root/levels"
printf 'level: 1\ntitle: t\ngoal: g\ncheck: file ../escape\n' > "$scratch/root/levels/levels.txt"
PUZZLLM_ROOT="$scratch/root" "$bin" --check-level 1 2> "$scratch/err"; rc=$?
[ $rc -eq 2 ] && grep -q 'line 4: check file must be a plain' "$scratch/err" &&
    pass "bad level file reported" || fail "bad level file rc=$rc $(cat "$scratch/err")"

echo "test-cli: $fails failed"
[ $fails -eq 0 ]
