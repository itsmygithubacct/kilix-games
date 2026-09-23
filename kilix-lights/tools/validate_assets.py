#!/usr/bin/env python3
"""Validate committed visual/audio assets without mutating them."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import wave

from PIL import Image


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    checks = (
        (root / "assets/art/room.ppm", (640, 400), "RGB"),
        (root / "assets/art/switch.ppm", (96, 96), "RGB"),
        (root / "assets/art/switch-mask.ppm", (96, 96), "RGB"),
    )
    for path, size, mode in checks:
        with Image.open(path) as image:
            image.load()
            if image.size != size or image.mode != mode:
                raise SystemExit(f"invalid {path.name}: {image.size} {image.mode}")
            colors = image.getcolors(maxcolors=size[0] * size[1])
            if path.name == "room.ppm" and (colors is None or len(colors) < 256):
                raise SystemExit("room.ppm is unexpectedly flat")
        print(f"visual: {path.name} {size[0]}x{size[1]} sha256={digest(path)[:16]}")

    mask_path = root / "assets/art/switch-mask.ppm"
    with Image.open(mask_path) as mask_image:
        channels = mask_image.split()
        if channels[0].tobytes() != channels[1].tobytes() or channels[0].tobytes() != channels[2].tobytes():
            raise SystemExit("switch mask is not grayscale")
        histogram = channels[0].histogram()
        if histogram[0] < 100 or histogram[255] < 2_000 or sum(histogram[1:255]) < 10:
            raise SystemExit("switch mask lacks transparent/solid/antialiased pixels")

    wav_path = root / "assets/sfx/light-switch.wav"
    with wave.open(str(wav_path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 44_100):
            raise SystemExit("light-switch.wav must be mono PCM16 at 44.1kHz")
        frames = wav.getnframes()
        if not 4_000 <= frames <= 10_000:
            raise SystemExit(f"light-switch.wav has unexpected length: {frames}")
        payload = wav.readframes(frames)
        if payload == bytes(len(payload)):
            raise SystemExit("light-switch.wav is silent")
    print(f"audio: {wav_path.name} {frames} frames sha256={digest(wav_path)[:16]}")
    print("asset validation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
