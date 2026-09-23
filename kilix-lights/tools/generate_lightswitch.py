#!/usr/bin/env python3
"""Synthesize the original mechanical light-switch cue deterministically."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import struct
import wave


RATE = 44_100
DURATION = 0.155


def envelope(t: float, start: float, decay: float) -> float:
    if t < start:
        return 0.0
    return math.exp(-(t - start) * decay)


def synthesize() -> list[int]:
    count = round(RATE * DURATION)
    state = 0x4C494748
    previous_noise = 0.0
    values: list[float] = []

    for i in range(count):
        t = i / RATE
        state = (state * 1_664_525 + 1_013_904_223) & 0xFFFFFFFF
        noise = ((state >> 8) / 0xFFFFFF) * 2.0 - 1.0
        bright_noise = noise - previous_noise * 0.72
        previous_noise = noise

        first = envelope(t, 0.006, 72.0)
        second = envelope(t, 0.043, 118.0)
        settle = envelope(t, 0.086, 155.0)

        value = first * (
            0.38 * math.sin(2.0 * math.pi * 118.0 * (t - 0.006))
            + 0.26 * math.sin(2.0 * math.pi * 860.0 * (t - 0.006))
            + 0.22 * bright_noise
        )
        value += second * (
            0.27 * math.sin(2.0 * math.pi * 430.0 * (t - 0.043))
            + 0.24 * math.sin(2.0 * math.pi * 2_240.0 * (t - 0.043))
            + 0.18 * bright_noise
        )
        value += settle * (
            0.13 * math.sin(2.0 * math.pi * 690.0 * (t - 0.086))
            + 0.08 * bright_noise
        )
        # Gentle room-body resonance without extending the cue.
        value += 0.055 * first * math.sin(2.0 * math.pi * 182.0 * t)
        values.append(value)

    peak = max(abs(value) for value in values)
    scale = 0.78 / peak
    frames = [round(max(-0.95, min(0.95, value * scale)) * 32767) for value in values]

    fade = 96
    for i in range(fade):
        frames[i] = round(frames[i] * i / fade)
        frames[-1 - i] = round(frames[-1 - i] * i / fade)
    return frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    frames = synthesize()
    with wave.open(str(args.output), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(b"".join(struct.pack("<h", frame) for frame in frames))
    print(f"wrote {args.output} ({len(frames)} frames, {len(frames) / RATE:.3f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
