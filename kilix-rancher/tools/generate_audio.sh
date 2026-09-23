#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
OUT_DIR="$ROOT_DIR/assets/sfx"

if ! command -v sox >/dev/null 2>&1; then
    echo "generate_audio.sh: SoX is required (the 'sox' command was not found)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/kilix-audio.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT

# Disable dithering so every run produces byte-for-byte identical PCM.  All
# tones are synthesized here; the game ships no sampled or third-party audio.
SOX=(sox -D)
FORMAT=(-r 44100 -c 1 -b 16 -e signed-integer)

# Menu movement: a tiny, warm upward paw-step.
"${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/move.wav" \
    synth 0.075 triangle 360-610 lowpass 1500 gain -n -13 \
    fade q 0.002 0.075 0.025

# Confirmation: a clean two-note fire-spark chirp.
"${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/confirm-a.wav" \
    synth 0.070 sine 660 gain -n -11 fade q 0.003 0.070 0.018
"${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/confirm-b.wav" \
    synth 0.105 sine 990 gain -n -10 fade q 0.003 0.105 0.035
"${SOX[@]}" "$TMP_DIR/confirm-a.wav" "$TMP_DIR/confirm-b.wav" \
    "${FORMAT[@]}" "$TMP_DIR/confirm.wav"

# Training: a rising, slightly percussive effort pulse.
"${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/train.wav" \
    synth 0.280 square 115-390 gain -8 lowpass 1050 gain -n -14 \
    tremolo 18 45 fade q 0.004 0.280 0.055

# Battle hit: a compact downward impact without using a noise sample.
"${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/hit.wav" \
    synth 0.145 sawtooth 210-52 gain -4 lowpass 850 overdrive 7 gain -n -12 \
    fade q 0.001 0.145 0.050

# Victory: an original four-note ascending fanfare.
for spec in "c:523.25" "e:659.25" "g:783.99" "top:1046.50"; do
    label=${spec%%:*}
    frequency=${spec#*:}
    duration=0.105
    release=0.030
    if [[ $label == top ]]; then
        duration=0.220
        release=0.090
    fi
    "${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/win-$label.wav" \
        synth "$duration" sine "$frequency" gain -n -11 \
        fade q 0.003 "$duration" "$release"
done
"${SOX[@]}" "$TMP_DIR/win-c.wav" "$TMP_DIR/win-e.wav" \
    "$TMP_DIR/win-g.wav" "$TMP_DIR/win-top.wav" \
    "${FORMAT[@]}" "$TMP_DIR/win.wav"

# Defeat: three soft descending embers.
for spec in "a:392.00" "b:311.13" "c:220.00"; do
    label=${spec%%:*}
    frequency=${spec#*:}
    duration=0.160
    release=0.055
    if [[ $label == c ]]; then
        duration=0.260
        release=0.120
    fi
    "${SOX[@]}" -n "${FORMAT[@]}" "$TMP_DIR/lose-$label.wav" \
        synth "$duration" sine "$frequency" lowpass 1200 gain -n -14 \
        fade q 0.004 "$duration" "$release"
done
"${SOX[@]}" "$TMP_DIR/lose-a.wav" "$TMP_DIR/lose-b.wav" \
    "$TMP_DIR/lose-c.wav" "${FORMAT[@]}" "$TMP_DIR/lose.wav"

# Every effect was synthesized into the scratch dir; publish all six at once so
# a SoX failure partway through (set -e aborts) can never leave the shipped
# asset set half-regenerated or with one truncated WAV.
mv -f "$TMP_DIR/move.wav" "$TMP_DIR/confirm.wav" "$TMP_DIR/train.wav" \
    "$TMP_DIR/hit.wav" "$TMP_DIR/win.wav" "$TMP_DIR/lose.wav" "$OUT_DIR/"

echo "Generated six original sound effects in $OUT_DIR"
