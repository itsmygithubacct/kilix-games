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
"$bin" --goal-status > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx COMPLETE "$scratch/out" && grep -q '^Level 1 ' "$scratch/out" &&
    pass "current goal is complete" || fail "complete goal status rc=$rc"
printf 'level 2\n' > "$PUZZLLM_STATE/progress"
"$bin" --goal-status > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -qx INCOMPLETE "$scratch/out" && grep -q 'strawberry.txt' "$scratch/out" &&
    pass "current goal names remaining work" || fail "missing current-room feedback rc=$rc"
touch "$PUZZLLM_STATE/transition"
"$bin" --goal-status > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && grep -q 'Changing rooms' "$scratch/out" &&
    pass "a room transition reports a temporary status" || fail "graded an in-progress room change"
rm "$PUZZLLM_STATE/transition"
printf 'level 1\n' > "$PUZZLLM_STATE/progress"
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
"$bin" --check-level 99 2>/dev/null; rc=$?
[ $rc -eq 2 ] && pass "unknown level is an error" || fail "unknown level rc=$rc"
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

grep -q 'Hello, World' "$PUZZLLM_STATE/workspace/GOAL.md" &&
    grep -q 'Walk to the agent' "$PUZZLLM_STATE/workspace/HINTS.md" &&
    [ ! -e "$PUZZLLM_STATE/workspace/prior_levels" ] &&
    pass "rendering a different room keeps the active guides" || fail "render replaced a guide"

# the frame timer: a busy frame at the size given, and which renderer drew it
printf '%s\n' '{"schema_version":1,"benchmark":"OneBit","split":"dev","total_runs":0,"models":[]}' > "$scratch/saved rankings.json"
if "$bin" --yesno-compare "$scratch/saved rankings.json" > "$scratch/machine-table" &&
   grep -q 'Mean s/answer' "$scratch/machine-table"; then
    pass "--yesno-compare reads an exported ranking with a space in its path"
else
    fail "--yesno-compare did not export the timing columns"
fi

"$bin" --bench 1366x768 > "$scratch/bench"; rc=$?
[ $rc -eq 0 ] && grep -q '^renderer: ' "$scratch/bench" &&
    grep -q '^frame 1366x768: .* ms/frame average' "$scratch/bench" &&
    pass "--bench times a 1366x768 frame: $(grep '^frame' "$scratch/bench" | cut -d: -f2)" ||
    fail "--bench rc=$rc $(cat "$scratch/bench")"
"$bin" --bench big 2>/dev/null; rc=$?
[ $rc -eq 2 ] && pass "--bench with a bad size is an error" || fail "--bench big rc=$rc"
out=$(PUZZLLM_RENDER=raster "$bin" --renderer) && [ "$out" = "system font" ] &&
    pass "PUZZLLM_RENDER=raster turns Neuronte off" || fail "PUZZLLM_RENDER=raster: $out"
out=$(PUZZLLM_RENDER= "$bin" --renderer) && [ "$out" = "system font" ] &&
    pass "the system font is the default" || fail "default renderer: $out"
if grep -q '^neuronte' "$here/build/render.ident" 2>/dev/null; then
    printf 'on\n' > "$PUZZLLM_STATE/neuronte"
    out=$(PUZZLLM_RENDER= "$bin" --renderer)
    case $out in neuronte*) pass "the saved setting enables Neuronte" ;;
        *) fail "--renderer in a Neuronte build: $out" ;; esac
    out=$(PUZZLLM_RENDER= PUZZLLM_NEURONTE_WEIGHTS="$scratch/none.nrt" "$bin" --renderer)
    case $out in "system font")
        pass "missing weights fall back to the system font" ;;
        *) fail "missing weights: $out" ;; esac
    rm "$PUZZLLM_STATE/neuronte"
fi

# Resume the chest room, then solve it through a real encrypted archive.
printf 'level 9\n' > "$PUZZLLM_STATE/progress"
workspace=$PUZZLLM_STATE/workspace
"$bin" --check-level 9 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ -f "$workspace/chest.zip" ] &&
    pass "chest room creates a ZIP and awaits the key" || fail "chest setup rc=$rc"
printf '%032d\n' 0 > "$workspace/key.txt"
"$bin" --check-level 9 > "$scratch/out"; rc=$?
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
"$bin" --check-level 9 > "$scratch/out"; rc=$?
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
    grep -q '^old$' "$workspace/prior_levels/level_1/hello_world.txt" &&
    grep -q '^Hello, World!$' "$workspace/prior_levels/level_1/hello_world.txt.1" &&
    pass "interrupted transition resumes without overwriting archived files" ||
    fail "transition recovery rc=$rc"

# Room 8 ships the actual broken Bash script and a folder with tricky names.
printf 'level 8\n' > "$PUZZLLM_STATE/progress"
"$bin" --check-level 8 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q 'did not print the correct count for sample files' "$scratch/out" &&
    [ -f "$workspace/sample files/beta notes.txt" ] &&
    [ -f "$workspace/sample files/.hidden" ] && [ -f "$workspace/sample files/archive/old.txt" ] &&
    pass "room 8 ships the folder and rejects the broken script" || fail "room 8 setup rc=$rc"
printf 'echo 3\n' > "$workspace/count_files.sh"
"$bin" --check-level 8 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q 'must also work with other folder paths' "$scratch/out" &&
    pass "a hard-coded count fails the extra folder checks" || fail "room 8 constant rc=$rc"
cat > "$workspace/count_files.sh" <<'EOF'
folder=${1:-sample files}
find "$folder" -maxdepth 1 -mindepth 1 -type f -printf . | wc -c
EOF
"$bin" --check-level 8 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -q 'level 8: met' "$scratch/out" &&
    grep -q '^find ' "$workspace/count_files.sh" &&
    pass "a general fix opens room 8 and survives reopening the save" || fail "room 8 fix rc=$rc"
printf 'An extra file for the same script.\n' > "$workspace/sample files/new file.txt"
"$bin" --check-level 8 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && [ -f "$workspace/sample files/new file.txt" ] &&
    pass "the grader uses the actual shipped directory in the workspace" || fail "room 8 changed folder rc=$rc"

# The repaired script is archived when the player enters the chest room.
printf 'Keep this instruction across rooms.\n' > "$workspace/.system/llama-prompt.txt"
printf 'level 8\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 9 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -qx 'level 9' "$PUZZLLM_STATE/progress" &&
    [ -f "$workspace/prior_levels/level_8/count_files.sh" ] && [ -f "$workspace/chest.zip" ] &&
    pass "room 8 leads to the password-protected archive in room 9" ||
    fail "room 8 to room 9 transition rc=$rc"
grep -qx 'Keep this instruction across rooms.' "$workspace/.system/llama-prompt.txt" &&
    pass "prompt settings stay in the sandbox home across rooms" || fail "prompt settings were archived"

# A crash after archiving the departing room resumes the restore phase.
mkdir -p "$workspace/level_9"
mv "$workspace/chest.zip" "$workspace/level_9/chest.zip"
chest_sum=$(sha256sum < "$workspace/level_9/chest.zip")
printf 'restore-back 9\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 8 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'level 8' "$PUZZLLM_STATE/progress" &&
    [ -f "$workspace/count_files.sh" ] && [ ! -e "$PUZZLLM_STATE/transition" ] &&
    pass "interrupted backtrack restores room 8 work and progress" || fail "backtrack recovery rc=$rc"
printf 'level 8\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 9 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ "$(sha256sum < "$workspace/chest.zip")" = "$chest_sum" ] &&
    pass "returning to room 9 restores the original encrypted archive" || fail "room 9 restore rc=$rc"

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

printf 'level 6\n' > "$PUZZLLM_STATE/transition"
"$bin" --check-level 7 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -qx 'level 7' "$PUZZLLM_STATE/progress" &&
    [ -f "$workspace/prior_levels/level_6/report.txt" ] && [ -x "$workspace/run_check.sh" ] &&
    pass "room 7 archives the previous report and installs the shell check" || fail "room 7 setup rc=$rc"
latest=$(run_report)
filename=${latest#* }
stamp=$(printf %s "$filename" | tr -cd cfilorux | wc -c)
printf '%s %d\n' "$latest" "$stamp" > "$workspace/report.txt"
"$bin" --check-level 7 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && pass "room 7 accepts the stamped shell report" || fail "room 7 stamp rc=$rc"
newest=$(run_report)
"$bin" --check-level 7 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && [ "$latest" != "$newest" ] &&
    pass "room 7 closes the door when the report goes stale" || fail "room 7 stale rc=$rc"

# The keeper grades the script against hidden inputs.
printf 'level 10\n' > "$PUZZLLM_STATE/progress"
printf '%s\n' "printf '4\\n9\\n'" > "$workspace/keep.sh"
"$bin" --check-level 10 > "$scratch/out"; rc=$?
[ "$rc" -eq 1 ] && grep -q 'keep.sh' "$workspace/GOAL.md" &&
    pass "room 10 rejects a script that only prints the example" || fail "room 10 sample rc=$rc"
cat > "$workspace/keep.sh" <<'EOF'
awk '{for (i=1; i<=NF; i++) { if (seen && $i+0 > previous) print $i; previous=$i+0; seen=1 }}'
EOF
"$bin" --check-level 10 > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && pass "a working keeper opens room 10" || fail "room 10 keeper rc=$rc"

# A journal remembers whether the outgoing work was to be kept, even if the preference changes.
(
    export PUZZLLM_STATE="$scratch/keep-recovery"
    "$bin" --check-level 1 > /dev/null
    ws="$PUZZLLM_STATE/workspace"
    printf 'keep my notes\n' > "$ws/notes"
    printf 'keep-next 1\n' > "$PUZZLLM_STATE/transition"
    printf 'on\n' > "$PUZZLLM_STATE/level-cleanup"
    printf 'occupied\n' > "$ws/prior_levels"
    "$bin" --check-level 2 > /dev/null 2>&1
    [ "$?" -eq 2 ] && [ -f "$PUZZLLM_STATE/transition" ] &&
        grep -q 'Hello, World' "$ws/GOAL.md" && grep -q 'Walk to the agent' "$ws/HINTS.md" || exit 1
    mv "$ws/prior_levels" "$ws/occupied"
    "$bin" --check-level 2 > /dev/null
    [ "$?" -eq 1 ] && grep -qx 'level 2' "$PUZZLLM_STATE/progress" &&
        [ ! -e "$PUZZLLM_STATE/transition" ] && grep -qx 'keep my notes' "$ws/notes" &&
        grep -q 'Hello, World' "$ws/prior_levels/GOAL_level_1.md" &&
        grep -q 'Walk to the agent' "$ws/prior_levels/HINTS_level_1.md" && grep -q strawberry "$ws/GOAL.md"
) && pass "blocked guide archiving preserves files and retries the original keep decision" ||
    fail "keep transition recovery"

# a broken level file is reported with its line
mkdir -p "$scratch/root/levels"
printf 'level: 1\ntitle: t\ngoal: g\ncheck: file ../escape\n' > "$scratch/root/levels/levels.txt"
PUZZLLM_ROOT="$scratch/root" "$bin" --check-level 1 2> "$scratch/err"; rc=$?
[ $rc -eq 2 ] && grep -q 'line 4: check file must be a plain' "$scratch/err" &&
    pass "bad level file reported" || fail "bad level file rc=$rc $(cat "$scratch/err")"

# Host downloads. The sandbox is not involved, and a bad name never reaches ollama.
fetch=$here/libexec/puzzllm-fetch
models=$scratch/fetch-models
mkdir -p "$scratch/bin" "$models"
cat > "$scratch/bin/ollama" <<'EOF'
#!/bin/sh
case "$1" in
    show) exit 1 ;;
esac
printf '%s\n' "$2" >> "$PUZZLLM_OLLAMA_LOG"
exit 0
EOF
chmod 755 "$scratch/bin/ollama" "$fetch"
export PUZZLLM_OLLAMA_LOG=$scratch/ollama.log PATH="$scratch/bin:$PATH"
"$fetch" ollama 'bad name' > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$PUZZLLM_OLLAMA_LOG" ] && grep -q 'Ollama names' "$scratch/out" &&
    pass "ollama pull rejects a name with a space" || fail "bad ollama name rc=$rc"
"$fetch" ollama '../evil' > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$PUZZLLM_OLLAMA_LOG" ] &&
    pass "ollama pull rejects a dotted path" || fail "dotted ollama name rc=$rc"
"$fetch" ollama 'qwen3:0.6b' > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'pulled qwen3:0.6b' "$scratch/out" &&
    grep -qx 'qwen3:0.6b' "$PUZZLLM_OLLAMA_LOG" &&
    pass "ollama pull runs on the host with the typed name" || fail "ollama pull rc=$rc"
payload=$scratch/payload.gguf
printf 'puzzllm-fetch-ok\n' > "$payload"
bytes=$(wc -c < "$payload" | tr -d ' ')
sha=$(sha256sum "$payload" | awk '{print $1}')
url=file://$payload
KILIX_LLM_MODELS_DIR=$models PUZZLLM_FETCH_FIXTURE_ID=fixture \
    PUZZLLM_FETCH_FIXTURE_FILE=tiny.gguf PUZZLLM_FETCH_FIXTURE_BYTES=$bytes \
    PUZZLLM_FETCH_FIXTURE_SHA=$sha PUZZLLM_FETCH_FIXTURE_URL=$url \
    "$fetch" gguf fixture > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'saved tiny.gguf' "$scratch/out" &&
    [ "$(cat "$models/tiny.gguf")" = "puzzllm-fetch-ok" ] &&
    pass "a suggested GGUF is saved into the models folder" || fail "gguf fetch rc=$rc $(cat "$scratch/out")"
KILIX_LLM_MODELS_DIR=$models PUZZLLM_FETCH_FIXTURE_ID=fixture \
    PUZZLLM_FETCH_FIXTURE_FILE=tiny.gguf PUZZLLM_FETCH_FIXTURE_BYTES=$bytes \
    PUZZLLM_FETCH_FIXTURE_SHA=$sha PUZZLLM_FETCH_FIXTURE_URL=$url \
    "$fetch" gguf fixture > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'already saved tiny.gguf' "$scratch/out" &&
    pass "a matching GGUF is not downloaded again" || fail "gguf present rc=$rc"
printf 'x' >> "$models/tiny.gguf"
KILIX_LLM_MODELS_DIR=$models PUZZLLM_FETCH_FIXTURE_ID=fixture \
    PUZZLLM_FETCH_FIXTURE_FILE=tiny.gguf PUZZLLM_FETCH_FIXTURE_BYTES=$bytes \
    PUZZLLM_FETCH_FIXTURE_SHA=$sha PUZZLLM_FETCH_FIXTURE_URL=$url \
    "$fetch" gguf fixture > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && grep -q 'does not match' "$scratch/out" &&
    pass "a different file with the suggested name is left in place" || fail "gguf mismatch rc=$rc"
diffusion=$scratch/fetch-diffusion
PUZZLLM_DIFFUSION_MODELS=$diffusion KILIX_LLM_MODELS_DIR=$models PUZZLLM_FETCH_FIXTURE_ID=fixture \
    PUZZLLM_FETCH_FIXTURE_FILE=tiny-diffusion.gguf PUZZLLM_FETCH_FIXTURE_BYTES=$bytes \
    PUZZLLM_FETCH_FIXTURE_SHA=$sha PUZZLLM_FETCH_FIXTURE_URL=$url \
    "$fetch" diffusion fixture > "$scratch/out"; rc=$?
[ "$rc" -eq 0 ] && grep -qx 'saved tiny-diffusion.gguf' "$scratch/out" &&
    [ "$(cat "$diffusion/tiny-diffusion.gguf")" = "puzzllm-fetch-ok" ] && [ ! -e "$models/tiny-diffusion.gguf" ] &&
    pass "a diffusion GGUF is saved into the diffusion folder" || fail "diffusion fetch rc=$rc $(cat "$scratch/out")"
out=$(env -u PUZZLLM_DIFFUSION_MODELS XDG_DATA_HOME="$scratch/xdg" PUZZLLM_FETCH_FIXTURE_ID=fixture \
    PUZZLLM_FETCH_FIXTURE_FILE=tiny-diffusion.gguf PUZZLLM_FETCH_FIXTURE_BYTES=$bytes \
    PUZZLLM_FETCH_FIXTURE_SHA=$sha PUZZLLM_FETCH_FIXTURE_URL=$url "$fetch" diffusion fixture); rc=$?
[ "$rc" -eq 0 ] && [ -f "$scratch/xdg/puzzllm/diffusion/tiny-diffusion.gguf" ] &&
    pass "diffusion downloads default to the folder kilix-chat and the Bench search" || fail "diffusion default rc=$rc $out"
# A stand-in curl keeps a broken id check from starting a real download.
mkdir -p "$scratch/nocurl"
printf '#!/bin/sh\nexit 7\n' > "$scratch/nocurl/curl"
chmod 755 "$scratch/nocurl/curl"
PATH="$scratch/nocurl:$PATH" PUZZLLM_DIFFUSION_MODELS=$diffusion "$fetch" diffusion qwen3.5-0.8b > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && grep -qx 'unknown diffusion model' "$scratch/out" &&
    pass "a chat GGUF id is not a diffusion download" || fail "diffusion chat id rc=$rc"
for id in llada-moe-7b-a1b llada-1.5 dream-coder-7b d3llm-llada; do
    grep -q "^        $id)\$" "$fetch" || fail "no pinned diffusion download $id"
done
"$fetch" gguf nope > "$scratch/out"; rc=$?
[ "$rc" -eq 2 ] && grep -q 'unknown llama.cpp model' "$scratch/out" &&
    pass "an unknown GGUF id is refused" || fail "unknown gguf rc=$rc"
! grep -q 'bwrap' "$fetch" && pass "the fetcher does not enter the sandbox" ||
    fail "fetcher mentions the sandbox"

kx=$here/libexec/puzzllm-kilix
! grep -F '/.local/gpu_terminal' "$kx" &&
    pass "the Kilix launcher does not name the desktop storage tree" ||
    fail "launcher names the desktop storage tree"
"$kx" paths > "$scratch/kxpaths"; rc=$?
[ "$rc" -eq 0 ] &&
    grep -qx "host_runtime=$PUZZLLM_STATE/kx/b" "$scratch/kxpaths" &&
    grep -qx "host_listen=$PUZZLLM_STATE/kx/listen.sock" "$scratch/kxpaths" &&
    grep -qx "host_bridge=$PUZZLLM_STATE/kx/bridge.sock" "$scratch/kxpaths" &&
    grep -qx "listen=unix:/opt/puzzllm/kx/listen.sock" "$scratch/kxpaths" &&
    grep -qx "gpu_terminal_home=/opt/puzzllm/kx/g" "$scratch/kxpaths" &&
    [ "$(stat -c %a "$PUZZLLM_STATE/kx")" = 700 ] &&
    [ "$(stat -c %a "$PUZZLLM_STATE/kx/b")" = 700 ] &&
    pass "Kilix paths stay under the puzzllm state directory" ||
    fail "kilix paths rc=$rc $(cat "$scratch/kxpaths")"
cat > "$scratch/fake-kilix" <<'EOF'
#!/bin/sh
env
EOF
chmod 755 "$scratch/fake-kilix"
KITTY_LISTEN_ON=unix:@kilix-1516703 \
    KITTY_WINDOW_ID=18 \
    KITTY_PTY_BROKER_SESSION=desktop \
    GPU_TERMINAL_HOME=$scratch/forbidden/.local/gpu_terminal \
    PUZZLLM_KILIX=$scratch/fake-kilix \
    "$kx" session > "$scratch/kxenv"; rc=$?
[ "$rc" -eq 0 ] &&
    grep -qx "GPU_TERMINAL_HOME=$PUZZLLM_STATE/kx/g" "$scratch/kxenv" &&
    grep -qx "KITTY_PTY_BROKER_RUNTIME=$PUZZLLM_STATE/kx/b" "$scratch/kxenv" &&
    grep -qx "KITTY_LISTEN_ON=unix:$PUZZLLM_STATE/kx/listen.sock" "$scratch/kxenv" &&
    ! grep -F '/.local/gpu_terminal' "$scratch/kxenv" &&
    ! grep -F 'unix:@kilix' "$scratch/kxenv" &&
    ! grep -q '^KITTY_WINDOW_ID=' "$scratch/kxenv" &&
    ! grep -q '^KITTY_PTY_BROKER_SESSION=' "$scratch/kxenv" &&
    pass "a Kilix session does not attach to the desktop instance" ||
    fail "kilix session rc=$rc $(cat "$scratch/kxenv")"
PUZZLLM_KILIX_LISTEN=unix:@kilix-1 KITTY_LISTEN_ON=unix:@kilix-1 \
    "$kx" send shell </dev/null > "$scratch/out" 2>&1; rc=$?
[ "$rc" -eq 2 ] && grep -q 'not this sandbox Kilix' "$scratch/out" &&
    pass "Kilix send refuses a desktop listen socket" ||
    fail "kilix send rc=$rc $(cat "$scratch/out")"

# Chat with a model: kilix-chat opens in a new tab of the game's Kilix.
mkdir -p "$scratch/chat"
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "%s/chat/kitten-args"\n' "$scratch" > "$scratch/chat/kitten"
printf '#!/bin/sh\nexit 0\n' > "$scratch/chat/kilix-chat"
chmod 755 "$scratch/chat/kitten" "$scratch/chat/kilix-chat"
out=$(env -u KILIX_CHAT_MODELS -u OLLAMA_HOST KITTY_LISTEN_ON=unix:@host-kilix PUZZLLM_KITTEN="$scratch/chat/kitten" \
      PUZZLLM_KILIX_CHAT="$scratch/chat/kilix-chat" PUZZLLM_GPU=Vulkan1 "$here/libexec/puzzllm-chat"); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = "Opened kilix-chat in a new tab." ] &&
    [ "$(tr '\n' ' ' < "$scratch/chat/kitten-args")" = "@ --to unix:@host-kilix launch --type=tab --tab-title=kilix-chat --cwd=$HOME --env PUZZLLM_GPU=Vulkan1 $(readlink -f "$scratch/chat/kilix-chat") " ] &&
    pass "Chat opens kilix-chat in a new tab with the GPU setting" ||
    fail "chat rc=$rc $out $(cat "$scratch/chat/kitten-args" 2>/dev/null)"
rm -f "$scratch/chat/kitten-args"
out=$(env -u KITTY_LISTEN_ON PUZZLLM_KITTEN="$scratch/chat/kitten" PUZZLLM_KILIX_CHAT="$scratch/chat/kilix-chat" \
      "$here/libexec/puzzllm-chat"); rc=$?
[ "$rc" -eq 1 ] && [ "$out" = "Chat opens in a Kilix tab. Outside Kilix, run kilix-chat in a terminal." ] &&
    [ ! -e "$scratch/chat/kitten-args" ] && pass "Chat outside Kilix explains how to run kilix-chat" ||
    fail "chat outside Kilix rc=$rc $out"
out=$(PATH=/usr/bin:/bin PUZZLLM_KILIX_CHAT="$scratch/chat/absent" KITTY_LISTEN_ON=unix:@host-kilix PUZZLLM_KITTEN="$scratch/chat/kitten" \
      "$here/libexec/puzzllm-chat"); rc=$?
[ "$rc" -eq 1 ] && [ "$out" = "Install kilix-chat to chat with a model." ] && [ ! -e "$scratch/chat/kitten-args" ] &&
    pass "Chat without kilix-chat says to install it" || fail "chat without kilix-chat rc=$rc $out"
out=$(env -u PUZZLLM_KILIX_CHAT PATH=/usr/bin:/bin KITTY_LISTEN_ON=unix:@host-kilix PUZZLLM_KITTEN="$scratch/chat/kitten" \
      "$here/libexec/puzzllm-chat"); rc=$?
[ "$rc" -eq 0 ] && grep -qx "$(readlink -f "$here/third_party/kilix-chat/kilix-chat")" "$scratch/chat/kitten-args" &&
    pass "Chat runs the kilix-chat bundled in third_party" || fail "bundled chat rc=$rc $out"
rm -f "$scratch/chat/kitten-args"
printf '#!/bin/sh\nexit 1\n' > "$scratch/chat/kitten"
out=$(KITTY_LISTEN_ON=unix:@host-kilix PUZZLLM_KITTEN="$scratch/chat/kitten" \
      PUZZLLM_KILIX_CHAT="$scratch/chat/kilix-chat" "$here/libexec/puzzllm-chat"); rc=$?
[ "$rc" -eq 1 ] && [ "$out" = "Could not open a Kilix tab for kilix-chat." ] &&
    pass "a refused tab launch is reported" || fail "refused chat launch rc=$rc $out"

echo "test-cli: $fails failed"
[ $fails -eq 0 ]
