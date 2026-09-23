#!/bin/bash
# Regenerates kilix-pong's neural player end to end:
#   round 0   planner demonstrations (matches vs EASY/NORMAL/HARD in turn)
#   rounds 1+ DAgger: the model plays, the planner takes over w.p. 0.5^k,
#             every visited state is planner-labelled, data aggregated
#   select    by dev tournament (frozen rule below), then pack the blob
#
#   WORK=/path/outside/repo PYTHON=/path/to/python-with-torch \
#       tools/neural/train-policy.sh
#
# Seeds: training 1.. and 100000*k.., dev 8000000.. (never trained on).
# Selection rule (fixed before any run): most dev match wins vs HARD, then
# higher dev point difference vs HARD, then the earlier round.
# The shipped blob is then installed with tools/neural/install-policy.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${WORK:?set WORK to a scratch directory outside the repository}"
PYTHON=${PYTHON:-python3}
JOBS=${JOBS:-8}
ROUNDS=${ROUNDS:-2}
HIDDEN=${HIDDEN:-64}
TRAIN_MATCHES=${TRAIN_MATCHES:-240}   # per round
DEV_MATCHES=${DEV_MATCHES:-30}        # per level
LAB=./tools/neural/pong-lab
mkdir -p "$WORK/data" "$WORK/models"
make -s lab

# collect NAME POLICY-ARGS... : TRAIN_MATCHES matches in JOBS shards, mixed levels
collect() {
    local name=$1 seed=$2; shift 2
    local per=$(( (TRAIN_MATCHES + JOBS - 1) / JOBS )) pids=()
    for j in $(seq 0 $((JOBS - 1))); do
        # shard start is a multiple of 3, so level = game % 3 stays balanced
        nice "$LAB" "$@" --level mix --seed $((seed + j * per * 3)) --games "$per" \
            --dump "$WORK/data/$name-$j.bin" > "$WORK/data/$name-$j.json" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
    cat "$WORK/data/$name"-*.json
}

# dev MODEL : DEV_MATCHES per level on dev seeds, one JSON line each
dev() {
    for level in easy normal hard; do
        nice "$LAB" --left neural --weights "$1" --level "$level" --seed 8000000 \
            --games "$DEV_MATCHES" &
    done
    wait
}

[ -s "$WORK/data/dev-planner.bin" ] || \
    nice "$LAB" --left planner --level mix --seed 8000000 --games 30 \
        --dump "$WORK/data/dev-planner.bin" > "$WORK/data/dev-planner.json"

train=()
[ -s "$WORK/data/r0-0.bin" ] || collect r0 1 --left planner
train+=("$WORK/data"/r0-*.bin)
"$PYTHON" -B tools/neural/train.py --train "${train[@]}" --dev "$WORK/data/dev-planner.bin" \
    --hidden "$HIDDEN" --epochs 8 --out "$WORK/models/r0.kxpol" | tail -1
dev "$WORK/models/r0.kxpol" | tee "$WORK/models/r0-dev.jsonl"

for k in $(seq 1 "$ROUNDS"); do
    beta=$("$PYTHON" -c "print(0.5**$k)")
    collect "r$k" $((100000 * k)) --left mix --beta "$beta" --weights "$WORK/models/r$((k-1)).kxpol"
    train+=("$WORK/data/r$k"-*.bin)
    "$PYTHON" -B tools/neural/train.py --train "${train[@]}" --dev "$WORK/data/dev-planner.bin" \
        --hidden "$HIDDEN" --epochs 4 --init "$WORK/models/r$((k-1)).kxpol" \
        --out "$WORK/models/r$k.kxpol" | tail -1
    dev "$WORK/models/r$k.kxpol" | tee "$WORK/models/r$k-dev.jsonl"
done

"$PYTHON" - "$WORK/models" "$ROUNDS" <<'PY'
import json, sys
models, rounds = sys.argv[1], int(sys.argv[2])
best = None
for k in range(rounds + 1):
    rows = {json.loads(l)["level"]: json.loads(l) for l in open(f"{models}/r{k}-dev.jsonl")}
    hard = rows["hard"]
    key = (hard["wins"], hard["points_for"] - hard["points_against"], -k)
    print(f"r{k}: " + "  ".join(f"{lv} {r['wins']}/{r['games']} ({r['points_for']}-{r['points_against']})"
                               for lv, r in sorted(rows.items())))
    if best is None or key > best[0]:
        best = (key, k)
print(f"selected r{best[1]}")
open(f"{models}/SELECTED", "w").write(f"r{best[1]}\n")
PY
