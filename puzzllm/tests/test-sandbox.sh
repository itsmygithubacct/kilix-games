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
trap 'if [ -n "${ollama_pid:-}" ]; then kill "$ollama_pid" 2>/dev/null; wait "$ollama_pid" 2>/dev/null; fi; kill "$listener" 2>/dev/null; wait "$listener" 2>/dev/null' EXIT
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

# A fake CLI exercises the Codex launch and game-specific device sign-in path.
rm -f "$PUZZLLM_STATE/workspace/hello_world.txt"
if PUZZLLM_CODEX="$here/tests/fake-codex" timeout 15 "$sandbox" codex \
        < /dev/null > "$scratch/codex.log" 2>&1 &&
    grep -q 'fake-codex: wrote the workspace file' "$scratch/codex.log" &&
    [ -f "$PUZZLLM_STATE/codex/mock-signed-in" ] &&
    PUZZLLM_ROOT=$here "$here/build/puzzllm" --check-level 1 >/dev/null; then
    pass "Codex signs in to separate game state and completes a workspace goal"
else
    fail "Codex agent chain: $(tail -n 10 "$scratch/codex.log" | tr '\n' ' ')"
fi

# Ollama is exposed to the agent through a model-specific bridge only.
python3 "$here/tests/fake-ollama.py" "$scratch/ollama-port" > "$scratch/ollama-host.log" 2>&1 &
ollama_pid=$!
for _ in $(seq 50); do [ -s "$scratch/ollama-port" ] && break; sleep 0.1; done
export OLLAMA_HOST="127.0.0.1:$(cat "$scratch/ollama-port")"
found=$("$here/libexec/puzzllm-ollama" list)
if [ "$found" = "$(printf '%s\n' 'test/agent:7b' 'test/always:latest' 'test/other:latest')" ]; then
    pass "Ollama discovery lists installed local models only"
else
    fail "Ollama discovery returned: $found"
fi
if PUZZLLM_OMP=$here/tests/fake-ollama-omp PUZZLLM_OLLAMA_MODEL=test/agent:7b PUZZLLM_THINK=0 \
        timeout 20 "$sandbox" agent < /dev/null > "$scratch/ollama-agent.log" 2>&1 &&
        grep -q 'completion received' "$scratch/ollama-agent.log"; then
    pass "Ollama selection streams the chosen model and rejects other requests"
else
    fail "Ollama agent chain: $(tail -n 10 "$scratch/ollama-agent.log" | tr '\n' ' ')"
fi

details=$("$here/libexec/puzzllm-ollama" list-details)
if [ "$details" = "$(printf '%s\n' '1test/agent:7b' '2test/always:latest' '0test/other:latest')" ]; then
    pass "Ollama discovery identifies thinking support"
else
    fail "Ollama capabilities returned: $details"
fi
for model in test/agent:7b test/other:latest; do
    if PUZZLLM_OMP=$here/tests/fake-ollama-omp PUZZLLM_OLLAMA_MODEL=$model PUZZLLM_THINK=1 \
            timeout 20 "$sandbox" agent < /dev/null > "$scratch/ollama-thinking.log" 2>&1 &&
            grep -q 'completion received' "$scratch/ollama-thinking.log"; then
        pass "Ollama bridge enforces thinking support for $model"
    else
        fail "Ollama thinking for $model: $(tail -n 10 "$scratch/ollama-thinking.log" | tr '\n' ' ')"
    fi
done

if PUZZLLM_OMP=$here/tests/fake-ollama-omp PUZZLLM_OLLAMA_MODEL=test/always:latest PUZZLLM_THINK=0 \
        timeout 20 "$sandbox" agent < /dev/null > "$scratch/ollama-minimum.log" 2>&1 &&
        grep -q 'completion received with effort low' "$scratch/ollama-minimum.log"; then
    pass "Ollama uses minimum effort for an always-thinking model when off"
else
    fail "Ollama minimum: $(tail -n 10 "$scratch/ollama-minimum.log" | tr '\n' ' ')"
fi

echo "test-sandbox: $fails failed"
[ "$fails" -eq 0 ]
