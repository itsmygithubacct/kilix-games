#!/usr/bin/env python3
"""Prepare fixed-resolution P6 runtime art from the generated PNG sources."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageOps


CANVAS = (640, 400)
SWITCH = (96, 96)


def save_ppm(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(path, format="PPM")


def prepare_room(source: Path, output: Path) -> None:
    with Image.open(source) as image:
        room = ImageOps.fit(
            image.convert("RGB"), CANVAS, Image.Resampling.LANCZOS, centering=(0.5, 0.48)
        )
    save_ppm(room, output)


def prepare_switch(source: Path, rgb_output: Path, mask_output: Path) -> None:
    with Image.open(source) as image:
        rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    bbox = alpha.getbbox()
    if bbox is None:
        raise ValueError("switch source has no visible pixels")
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    pad = max(12, round(max(width, height) * 0.035))
    side = max(width, height) + pad * 2
    cx = (left + right) // 2
    cy = (top + bottom) // 2
    crop_box = (cx - side // 2, cy - side // 2, cx - side // 2 + side, cy - side // 2 + side)
    cropped = rgba.crop(crop_box).resize(SWITCH, Image.Resampling.LANCZOS)
    crop_alpha = cropped.getchannel("A")
    rgb = cropped.convert("RGB")
    black = Image.new("RGB", SWITCH, (0, 0, 0))
    black.paste(rgb, mask=crop_alpha)
    save_ppm(black, rgb_output)
    save_ppm(Image.merge("RGB", (crop_alpha, crop_alpha, crop_alpha)), mask_output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    prepare_room(root / "assets/source/electrical-workshop.png", root / "assets/art/room.ppm")
    prepare_switch(
        root / "assets/source/vintage-switch-alpha.png",
        root / "assets/art/switch.ppm",
        root / "assets/art/switch-mask.ppm",
    )
    print("prepared room.ppm (640x400) and switch layers (96x96)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
