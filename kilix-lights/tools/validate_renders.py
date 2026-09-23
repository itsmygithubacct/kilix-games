#!/usr/bin/env python3
"""Check that the complete deterministic render-review set is valid P6 art."""

from __future__ import annotations

from pathlib import Path
import sys

from PIL import Image


EXPECTED = {
    "classic.ppm",
    "classic-hover.ppm",
    "starter.ppm",
    "expert.ppm",
    "help.ppm",
    "victory.ppm",
}


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_renders.py DIRECTORY")
    directory = Path(sys.argv[1])
    actual = {path.name for path in directory.glob("*.ppm")}
    if actual != EXPECTED:
        raise SystemExit(f"render set mismatch: expected {sorted(EXPECTED)}, got {sorted(actual)}")
    for name in sorted(EXPECTED):
        with Image.open(directory / name) as image:
            image.load()
            if image.size != (640, 400) or image.mode != "RGB":
                raise SystemExit(f"invalid render {name}: {image.size} {image.mode}")
            colors = image.getcolors(maxcolors=640 * 400)
            if colors is None or len(colors) < 512:
                raise SystemExit(f"render {name} is unexpectedly flat")
        print(f"render: {name} 640x400 full-color")
    print("render validation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
