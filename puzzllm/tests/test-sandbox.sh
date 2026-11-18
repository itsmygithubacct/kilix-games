#!/bin/bash
# test-sandbox.sh SCRATCH - confinement and the agent chain, no real model.
#
#  1. probes inside the agent and shell sandboxes (tests/sandbox-probe.sh):
#     writes outside the workspace fail, the network is closed, the host home
#     is invisible, and every writable mount is an allowed one;
#  2. host-side checks that nothing landed outside the state folder;
#  3. the full agent chain with a fake llama-server and a fake omp: models.yml,
#     the server on the sandbox loopback, omp in the workspace, the file made,
#     and the level-1 check met;
#  4. the workspace survives a sandbox restart.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
scratch=${1:?usage: test-sandbox.sh SCRATCH}
sandbox=$here/libexec/puzzllm-sandbox
fails=0
pass() { printf 'ok   %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }

command -v bwrap >/dev/null || { echo "test-sandbox: bwrap is required"; exit 1; }
rm -rf "$scratch"
mkdir -p "$scratch/outside"
export PUZZLLM_STATE=$scratch/state

# A listener on the host's loopback that the sandboxes must not reach.
port_file=$scratch/port
python3 -I -c '
import socket, sys
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(8)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
while True:
    c, _ = s.accept(); c.close()
' "$port_file" &
listener=$!
trap 'kill "$listener" 2>/dev/null; wait "$listener" 2>/dev/null' EXIT
for _ in $(seq 50); do [ -s "$port_file" ] && break; sleep 0.1; done
port=$(cat "$port_file")
if timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
    pass "host listener is reachable from the host (control)"
else
    fail "host listener not reachable even from the host"
fi

for mode in agent shell; do
    if "$sandbox" "$mode-exec" -- bash -s -- "$mode" "$scratch/outside" "$port" "$HOME" \
            < "$here/tests/sandbox-probe.sh"; then
        pass "$mode sandbox probe"
    else
        fail "$mode sandbox probe"
    fi
done

# host side: nothing escaped
if [ -z "$(ls -A "$scratch/outside")" ]; then pass "outside folder untouched"
else fail "outside folder has: $(ls -A "$scratch/outside")"; fi
[ ! -e "$scratch/escape-agent" ] && [ ! -e "$scratch/escape-shell" ] &&
    pass "no escape files beside the state" || fail "escape file beside the state"
for who in agent shell; do
    [ "$(cat "$PUZZLLM_STATE/workspace/written-by-$who" 2>/dev/null)" = "$who" ] &&
        pass "$who's workspace write is on the host" || fail "$who's workspace write missing"
done

# the agent chain, with stand-ins for llama-server and omp
rm -f "$PUZZLLM_STATE/workspace/hello_world.txt"
PUZZLLM_OMP=$here/tests/fake-omp \
PUZZLLM_LLAMA_SERVER=$here/tests/fake-llama/llama-server \
PUZZLLM_MODEL=$here/tests/fake-llama/model.gguf \
    timeout 60 "$sandbox" agent < /dev/null > "$scratch/agent.log" 2>&1
if grep -q 'fake-omp: wrote hello_world.txt' "$scratch/agent.log"; then
    pass "agent chain ran"
else
    fail "agent chain: $(tail -n 5 "$scratch/agent.log" | tr '\n' ' ')"
fi
[ "$(cat "$PUZZLLM_STATE/workspace/hello_world.txt" 2>/dev/null)" = "Hello, World!" ] &&
    pass "hello_world.txt made through the sandbox's model" ||
    fail "hello_world.txt not made"
if PUZZLLM_ROOT=$here "$here/build/puzzllm" --check-level 1 >/dev/null; then
    pass "level 1 goal met"
else
    fail "level 1 goal not met"
fi
models=$PUZZLLM_STATE/agent/models.yml
if grep -q 'baseUrl: http://127.0.0.1:' "$models" &&
   [ "$(grep -c 'baseUrl:' "$models")" = 1 ]; then
    pass "omp gets exactly one provider, on the sandbox loopback"
else
    fail "models.yml: $(grep baseUrl "$models")"
fi
grep -qx 'setupVersion: 2' "$PUZZLLM_STATE/agent/config.yml" &&
    pass "omp's first-run wizard is marked done" || fail "no setupVersion in config.yml"
grep -q 'ready; starting omp' "$PUZZLLM_STATE/agent/puzzllm/status" &&
    pass "status file written for the activity screen" || fail "no status file"

# persistence: a new sandbox sees the earlier work
if [ "$("$sandbox" agent-exec -- cat hello_world.txt 2>/dev/null)" = "Hello, World!" ] &&
   [ "$("$sandbox" shell-exec -- cat hello_world.txt 2>/dev/null)" = "Hello, World!" ]; then
    pass "workspace persists into new agent and shell sandboxes"
else
    fail "workspace did not persist"
fi
# the shell can finish a goal by hand (owner's decision)
rm -f "$PUZZLLM_STATE/workspace/hello_world.txt"
"$sandbox" shell-exec -- sh -c 'echo "hello world" > hello_world.txt'
if PUZZLLM_ROOT=$here "$here/build/puzzllm" --check-level 1 >/dev/null; then
    pass "goal met by hand from the shell"
else
    fail "shell could not meet the goal by hand"
fi

echo "test-sandbox: $fails failed"
[ "$fails" -eq 0 ]
