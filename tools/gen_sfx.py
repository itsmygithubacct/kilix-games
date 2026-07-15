#!/usr/bin/env python3
"""Deterministic sound bank generator for kilix-pong.

Renders the 21 WAVs in assets/sfx/ (7 cues x 3 variants). Stdlib only: no
numpy, no scipy, no samples, no recordings. Every sound here is synthesised
from scratch.

Why the arithmetic looks the way it does
----------------------------------------
The whole sample path is integer fixed-point, and there is not a single call to
math.sin/exp/log/pow anywhere in it. That is deliberate and load-bearing.

IEEE-754 specifies +, -, *, / as correctly rounded, so they produce bit-identical
results on every conforming platform. Transcendentals are *not* specified that
way: math.sin and friends go straight to the platform libm and differ, in the
last place, between implementations and versions. Since `--check` byte-compares
a fresh render against the committed WAVs, a single math.sin in the signal path
would turn a different libm into a spurious CI failure -- and would make the
claim "this generator reproduces the shipped bank" false the moment it left the
machine that authored it.

So: sine comes from a table built by an integer Taylor series over Python's
exact big integers, envelopes are integer polynomials rather than exp() decays,
and noise is a fixed-seed xorshift32. Same bytes, any box, forever.

Usage:
  gen_sfx.py --out assets/sfx --manifest docs/audio-provenance.json
  gen_sfx.py --check   # render in memory, byte-compare, validate, reconcile hashes
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import wave
from pathlib import Path

SR = 44100
CHANNELS = 1
SAMPWIDTH = 2

# Fixed-point scale for the DSP. 2**30 leaves comfortable headroom for the
# intermediate products below without ever needing a float.
BITS = 30
ONE = 1 << BITS

# Phase accumulator resolution, and the sine table it indexes.
PHASE_BITS = 24
PHASE_ONE = 1 << PHASE_BITS          # one full period
TABLE_BITS = 12
TABLE_N = 1 << TABLE_BITS            # 4096 entries

# pi in BITS fixed point, written as a literal so no libm is ever consulted.
# round(pi * 2**30) == 3373259426
PI_FX = 3373259426
TWO_PI_FX = 2 * PI_FX
HALF_PI_FX = PI_FX // 2


def _sin_fx(theta_fx: int) -> int:
    """sin(theta) in BITS fixed point, integer-only.

    Range-reduces into [0, pi/2] then evaluates the Taylor series
    x - x^3/3! + x^5/5! - x^7/7! + x^9/9! - x^11/11!. Python ints are exact and
    unbounded, and // is floor division, so this is bit-reproducible anywhere.
    Truncation error is a few parts in 2**30 -- far below int16 resolution.
    """
    t = theta_fx % TWO_PI_FX
    sign = 1
    if t >= PI_FX:                  # sin(pi + a) = -sin(a)
        t -= PI_FX
        sign = -1
    if t > HALF_PI_FX:              # sin(pi - a) = sin(a)
        t = PI_FX - t
    x2 = (t * t) // ONE
    power = t                       # x^1
    acc = t
    term_sign = -1
    for factorial in (6, 120, 5040, 362880, 39916800):
        power = (power * x2) // ONE  # x^3, x^5, x^7, x^9, x^11
        acc += term_sign * (power // factorial)
        term_sign = -term_sign
    return sign * acc


SINE_TABLE = [_sin_fx((i * TWO_PI_FX) // TABLE_N) for i in range(TABLE_N)]


def _sine(phase: int) -> int:
    """Linearly interpolated table lookup. phase is PHASE_BITS fixed point."""
    p = phase & (PHASE_ONE - 1)
    idx = p >> (PHASE_BITS - TABLE_BITS)
    frac = p & ((1 << (PHASE_BITS - TABLE_BITS)) - 1)
    a = SINE_TABLE[idx]
    b = SINE_TABLE[(idx + 1) & (TABLE_N - 1)]
    return a + ((b - a) * frac >> (PHASE_BITS - TABLE_BITS))


def _square(phase: int) -> int:
    """Naive square. Aliases, and that is the point -- it is the classic
    rectangular blip. The one-pole lowpass downstream tames the worst of it."""
    return ONE if (phase & (PHASE_ONE - 1)) < (PHASE_ONE // 2) else -ONE


def _triangle(phase: int) -> int:
    p = phase & (PHASE_ONE - 1)
    quarter = PHASE_ONE // 4
    if p < quarter:
        return (p * ONE) // quarter
    if p < 3 * quarter:
        return ONE - (((p - quarter) * 2 * ONE) // (2 * quarter))
    return -ONE + (((p - 3 * quarter) * ONE) // quarter)


def _phase_step(freq_mhz: int) -> int:
    """Phase increment per sample for a frequency in millihertz."""
    return (freq_mhz * PHASE_ONE) // (SR * 1000)


class Noise:
    """Fixed-seed xorshift32. Seeded per cue+variant so every render matches."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFF or 0x1234567

    def next(self) -> int:
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x & 0xFFFFFFFF
        # Map to roughly [-ONE, ONE]
        return ((self.state >> 8) * 2 * ONE // 0xFFFFFF) - ONE


def _env(i: int, n: int, attack: int, power: int) -> int:
    """Linear attack then (1-u)^power decay, in fixed point.

    A polynomial decay rather than exp(): it needs only multiplies, so it stays
    bit-exact, and power=3..4 is percussive enough that nothing is lost.
    """
    if i < attack:
        return (i * ONE) // attack if attack else ONE
    span = n - attack
    if span <= 0:
        return 0
    u = ((i - attack) * ONE) // span
    v = ONE - u
    e = ONE
    for _ in range(power):
        e = (e * v) // ONE
    return e


class Lowpass:
    """One-pole IIR, integer state. coeff is in BITS fixed point."""

    def __init__(self, coeff: int) -> None:
        self.coeff = coeff
        self.y = 0

    def step(self, x: int) -> int:
        self.y += ((x - self.y) * self.coeff) // ONE
        return self.y


def _sweep(i: int, n: int, f0_mhz: int, f1_mhz: int) -> int:
    """Linear frequency sweep, integer interpolation."""
    if n <= 1:
        return f0_mhz
    return f0_mhz + ((f1_mhz - f0_mhz) * i) // (n - 1)


def _ms(milliseconds: int) -> int:
    return (SR * milliseconds) // 1000


def _clip16(v: int) -> int:
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


# ---------------------------------------------------------------------------
# Cue renderers. Each returns a list of int16 samples.
#
# These are pong-like beeps of our own: rectangular blips with fast polynomial
# decays, in the spirit of the arcade original without copying its tables. The
# paddle/wall pair sits low and dry so a long rally never gets fatiguing; score
# and gameover are the only cues allowed to sing.
# ---------------------------------------------------------------------------

def cue_paddle(variant: int) -> list[int]:
    """Short bright rectangular tick. Pitch rises slightly per variant so a
    rally rotating through the three does not sound like one looped sample."""
    freq = (226_000, 239_000, 253_000)[variant]
    n = _ms(48)
    lp = Lowpass(int(ONE * 0.36))
    phase = 0
    step = _phase_step(freq)
    out = []
    for i in range(n):
        s = _square(phase)
        phase += step
        s = lp.step(s)
        e = _env(i, n, _ms(1), 3)
        out.append(_clip16((s * e // ONE) * 8600 // ONE))
    return out


def cue_wall(variant: int) -> list[int]:
    """Duller, lower cousin of the paddle tick, with a touch of noise for the
    'thud' transient. Deliberately less bright so the ear can tell the two
    apart without looking at the screen."""
    freq = (149_000, 157_000, 141_000)[variant]
    n = _ms(40)
    lp = Lowpass(int(ONE * 0.22))
    rng = Noise(0x51A7_C0DE + variant * 7717)
    phase = 0
    step = _phase_step(freq)
    out = []
    for i in range(n):
        s = _square(phase)
        phase += step
        transient = _env(i, _ms(4), 0, 2)
        s = (s * 3 // 4) + (rng.next() * transient // ONE // 5)
        s = lp.step(s)
        e = _env(i, n, _ms(1), 3)
        out.append(_clip16((s * e // ONE) * 7600 // ONE))
    return out


def cue_score(variant: int) -> list[int]:
    """Two-note rising figure. This is the reward, so it is the one cue with a
    clean sine on top of the square."""
    base = (392_000, 440_000, 349_000)[variant]
    note_n = _ms(90)
    out = []
    for note in range(2):  # root, then a fifth above
        freq = base if note == 0 else (base * 3) // 2
        phase = 0
        step = _phase_step(freq)
        lp = Lowpass(int(ONE * 0.5))
        for i in range(note_n):
            s = (_sine(phase) * 2 // 3) + (_square(phase) // 3)
            phase += step
            s = lp.step(s)
            e = _env(i, note_n, _ms(2), 2)
            out.append(_clip16((s * e // ONE) * 7200 // ONE))
    return out


def cue_serve(variant: int) -> list[int]:
    """Rising chirp: the ball is coming."""
    f0 = (180_000, 196_000, 165_000)[variant]
    f1 = f0 * 2
    n = _ms(110)
    lp = Lowpass(int(ONE * 0.45))
    phase = 0
    out = []
    for i in range(n):
        phase += _phase_step(_sweep(i, n, f0, f1))
        s = _triangle(phase)
        s = lp.step(s)
        e = _env(i, n, _ms(3), 2)
        out.append(_clip16((s * e // ONE) * 6800 // ONE))
    return out


def cue_miss(variant: int) -> list[int]:
    """Falling chirp: you lost the point. Longer decay than serve so it reads
    as a consequence rather than an event."""
    f0 = (220_000, 208_000, 233_000)[variant]
    f1 = f0 // 2
    n = _ms(260)
    lp = Lowpass(int(ONE * 0.3))
    phase = 0
    out = []
    for i in range(n):
        phase += _phase_step(_sweep(i, n, f0, f1))
        s = (_square(phase) // 2) + (_triangle(phase) // 2)
        s = lp.step(s)
        e = _env(i, n, _ms(2), 3)
        out.append(_clip16((s * e // ONE) * 7000 // ONE))
    return out


def cue_menu(variant: int) -> list[int]:
    """UI tick. Short, quiet, unobtrusive."""
    freq = (523_000, 587_000, 494_000)[variant]
    n = _ms(55)
    lp = Lowpass(int(ONE * 0.55))
    phase = 0
    step = _phase_step(freq)
    out = []
    for i in range(n):
        s = _square(phase)
        phase += step
        s = lp.step(s)
        e = _env(i, n, _ms(1), 4)
        out.append(_clip16((s * e // ONE) * 5200 // ONE))
    return out


def cue_gameover(variant: int) -> list[int]:
    """Descending three-note figure. The only long cue in the bank."""
    roots = (
        (330_000, 262_000, 196_000),
        (349_000, 277_000, 208_000),
        (311_000, 247_000, 185_000),
    )[variant]
    note_n = _ms(150)
    out = []
    for freq in roots:
        phase = 0
        step = _phase_step(freq)
        lp = Lowpass(int(ONE * 0.4))
        for i in range(note_n):
            s = (_sine(phase) // 2) + (_square(phase) // 2)
            phase += step
            s = lp.step(s)
            e = _env(i, note_n, _ms(3), 2)
            out.append(_clip16((s * e // ONE) * 7400 // ONE))
    return out


# Order matches the SFX_* enum in src/kilix_pong.h. Variant 1 is the base name;
# variants 2 and 3 take the _v02/_v03 suffix, matching the house convention that
# src/sound.c's bank loader expects.
CUES = [
    ("paddle", cue_paddle),
    ("wall", cue_wall),
    ("score", cue_score),
    ("serve", cue_serve),
    ("miss", cue_miss),
    ("menu", cue_menu),
    ("gameover", cue_gameover),
]
VARIANTS = 3


def variant_filename(cue: str, variant: int) -> str:
    return f"{cue}.wav" if variant == 0 else f"{cue}_v{variant + 1:02d}.wav"


def encode_wav(samples: list[int]) -> bytes:
    """Render to in-memory WAV bytes via the wave module."""
    import io

    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPWIDTH)
        w.setframerate(SR)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    return buf.getvalue()


def render_all() -> list[dict]:
    """Render the full bank in memory. Returns one record per artifact."""
    records = []
    for cue, fn in CUES:
        for variant in range(VARIANTS):
            samples = fn(variant)
            data = encode_wav(samples)
            records.append(
                {
                    "cue": cue,
                    "variant": variant + 1,
                    "filename": variant_filename(cue, variant),
                    "bytes": data,
                    "frames": len(samples),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            )
    return records


def build_manifest(records: list[dict], out_rel: str) -> dict:
    return {
        "method": "deterministic stdlib-only integer fixed-point synthesis",
        "generator": "tools/gen_sfx.py",
        "generator_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "reproducible": (
            "No libm transcendentals in the sample path; integer fixed-point "
            "phase/envelope and fixed-seed xorshift noise only. `gen_sfx.py "
            "--check` byte-compares a fresh render against these artifacts."
        ),
        "sample_rate": SR,
        "channels": CHANNELS,
        "bits_per_sample": SAMPWIDTH * 8,
        "cue_count": len(CUES),
        "variants_per_cue": VARIANTS,
        "artifact_count": len(records),
        "artifacts": [
            {
                "cue": r["cue"],
                "variant": r["variant"],
                "runtime_file": f"{out_rel}/{r['filename']}",
                "frames": r["frames"],
                "duration_seconds": round(r["frames"] / SR, 6),
                "sha256": r["sha256"],
            }
            for r in records
        ],
    }


def do_write(out_dir: Path, manifest_path: Path, out_rel: str) -> int:
    records = render_all()
    out_dir.mkdir(parents=True, exist_ok=True)
    for r in records:
        (out_dir / r["filename"]).write_bytes(r["bytes"])
    manifest = build_manifest(records, out_rel)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    total = sum(r["frames"] for r in records)
    print(f"wrote {len(records)} wavs to {out_dir} ({total} frames)")
    print(f"wrote manifest {manifest_path}")
    return 0


def do_check(out_dir: Path, manifest_path: Path) -> int:
    """Regenerate in memory and reconcile against what is committed.

    Checks, in order: file present, bytes identical to a fresh render, PCM
    parses via the wave module with the expected format and non-zero frames,
    and the manifest hash matches. Renders in memory rather than to a temp
    directory -- there is nothing to clean up and nothing to leak into the tree.
    """
    failures: list[str] = []
    records = render_all()

    if not manifest_path.exists():
        print(f"FAIL missing manifest {manifest_path}", file=sys.stderr)
        return 1
    manifest = json.loads(manifest_path.read_text())
    by_file = {a["runtime_file"].rsplit("/", 1)[-1]: a for a in manifest["artifacts"]}

    if manifest.get("artifact_count") != len(records):
        failures.append(
            f"manifest artifact_count={manifest.get('artifact_count')} "
            f"but generator renders {len(records)}"
        )

    for r in records:
        name = r["filename"]
        path = out_dir / name
        if not path.exists():
            failures.append(f"{name}: missing from {out_dir}")
            continue

        on_disk = path.read_bytes()
        if on_disk != r["bytes"]:
            failures.append(
                f"{name}: bytes differ from a fresh render "
                f"(on-disk sha={hashlib.sha256(on_disk).hexdigest()[:12]} "
                f"regenerated sha={r['sha256'][:12]})"
            )
            continue

        with wave.open(str(path), "rb") as w:
            if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (
                CHANNELS,
                SAMPWIDTH,
                SR,
            ):
                failures.append(
                    f"{name}: format is "
                    f"{w.getnchannels()}ch/{w.getsampwidth() * 8}bit/{w.getframerate()}Hz, "
                    f"expected {CHANNELS}ch/{SAMPWIDTH * 8}bit/{SR}Hz"
                )
            if w.getnframes() == 0:
                failures.append(f"{name}: zero frames")

        entry = by_file.get(name)
        if entry is None:
            failures.append(f"{name}: absent from manifest")
        elif entry["sha256"] != r["sha256"]:
            failures.append(f"{name}: manifest sha256 does not match render")

    stray = sorted(p.name for p in out_dir.glob("*.wav") if p.name not in
                   {r["filename"] for r in records})
    if stray:
        failures.append(f"unexpected wavs in {out_dir}: {', '.join(stray)}")

    if failures:
        for f in failures:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print(f"ok: {len(records)} wavs byte-identical to a fresh render, "
          f"PCM validated, manifest reconciled")
    return 0


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="Generate the kilix-pong sound bank.")
    ap.add_argument("--out", type=Path, default=root / "assets" / "sfx",
                    help="output directory for the WAV bank")
    ap.add_argument("--manifest", type=Path,
                    default=root / "docs" / "audio-provenance.json",
                    help="provenance manifest path")
    ap.add_argument("--check", action="store_true",
                    help="do not write; verify the committed bank reproduces")
    args = ap.parse_args(argv)

    if args.check:
        return do_check(args.out, args.manifest)
    return do_write(args.out, args.manifest, "assets/sfx")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
