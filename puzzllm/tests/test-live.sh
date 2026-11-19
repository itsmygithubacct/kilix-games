#!/bin/bash
# test-live.sh SCRATCH - level 1 with the real omp and the real Qwen 3.5.
#
# Not part of `make test`: it needs omp, a llama-server build and the Qwen
# 3.5 0.8B GGUF (see `libexec/puzzllm-sandbox paths`), and nothing is
# downloaded. omp gets the level-1 goal as a one-shot prompt in a fresh
# workspace, up to PUZZLLM_LIVE_TRIES times (default 3) because a 0.8B model
# does not always do what it says; the level check decides. Each try may
# take PUZZLLM_LIVE_TIMEOUT seconds (default 1200): on a busy CPU the model
# reads omp's first prompt (about 7,000 tokens) slowly.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
scratch=${1:?usage: test-live.sh SCRATCH}
tries=${PUZZLLM_LIVE_TRIES:-3}
limit=${PUZZLLM_LIVE_TIMEOUT:-1200}
sandbox=$here/libexec/puzzllm-sandbox
prompt='Use the write tool to create a file named hello_world.txt in the current directory. Its content must be exactly: Hello, World!'

rm -rf "$scratch"
mkdir -p "$scratch/outside"
export PUZZLLM_STATE=$scratch/state
"$sandbox" paths
for f in omp llama_server model; do
    v=$("$sandbox" paths | sed -n "s/^$f=//p")
    if [ -z "$v" ]; then echo "test-live: no $f on this machine; skipped"; exit 77; fi
done

for try in $(seq "$tries"); do
    rm -f "$PUZZLLM_STATE/workspace/hello_world.txt"
    start=$(date +%s)
    timeout "$limit" "$sandbox" agent -p "$prompt" < /dev/null > "$scratch/try$try.log" 2>&1
    took=$(( $(date +%s) - start ))
    if PUZZLLM_ROOT=$here "$here/build/puzzllm" --check-level 1; then
        echo "test-live: met on try $try of $tries in ${took}s"
        printf 'file: %s\n' "$(head -c 200 "$PUZZLLM_STATE/workspace/hello_world.txt")"
        grep -E 'prompt eval time|       eval time' \
            "$PUZZLLM_STATE/agent/puzzllm/llama-server.log" | tail -n 2
        [ -z "$(ls -A "$scratch/outside")" ] || { echo "test-live: wrote outside!"; exit 1; }
        exit 0
    fi
    echo "test-live: try $try not met after ${took}s; the model said:"
    tail -n 3 "$scratch/try$try.log"
done
echo "test-live: not met in $tries tries"
exit 1
