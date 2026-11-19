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
"$bin" --check-level 2 > "$scratch/out"; rc=$?
[ $rc -eq 0 ] && grep -q 'level 2: met' "$scratch/out" && pass "level 2 met by a million ones" ||
    fail "level 2 rc=$rc $(cat "$scratch/out")"
echo >> "$scratch/state/workspace/1.txt"
"$bin" --check-level 2 > "$scratch/out"; rc=$?
[ $rc -eq 1 ] && grep -q 'one too many: often a line ending' "$scratch/out" &&
    pass "level 2 not met with a line ending" || fail "level 2 newline rc=$rc $(cat "$scratch/out")"
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

# a broken level file is reported with its line
mkdir -p "$scratch/root/levels"
printf 'level: 1\ntitle: t\ngoal: g\ncheck: file ../escape\n' > "$scratch/root/levels/levels.txt"
PUZZLLM_ROOT="$scratch/root" "$bin" --check-level 1 2> "$scratch/err"; rc=$?
[ $rc -eq 2 ] && grep -q 'line 4: check file must be a plain' "$scratch/err" &&
    pass "bad level file reported" || fail "bad level file rc=$rc $(cat "$scratch/err")"

echo "test-cli: $fails failed"
[ $fails -eq 0 ]
