# Asset provenance and licensing

This file records the origin and redistribution terms of the non-code assets
shipped with Joustix. The runtime files in `assets/` are the complete canonical
release set. Generation sources, prompts, raw responses, reference conversions,
normalization scripts, and preview material are intentionally kept outside the
public game tree.

The images were created specifically for Joustix with OpenAI image generation
and are distributed under the repository's MIT License to the extent copyright
or related rights exist. OpenAI is a tool provider, not an author, sponsor, or
endorser of Joustix.

## Production images

| Runtime file | Content | Release processing |
|---|---|---|
| `assets/player.ppm` | Eight-pose gold knight and cream ostrich atlas | Selected, point-resized, chroma-key normalized |
| `assets/bounder.ppm` | Eight-pose rust raider and olive buzzard atlas | Selected, point-resized, chroma-key normalized |
| `assets/hunter.ppm` | Eight-pose pale-steel huntress and slate hawk atlas | Selected, point-resized, chroma-key normalized |
| `assets/shadow.ppm` | Eight-pose black-violet lord and dark vulture atlas | Selected, point-resized, chroma-key normalized |
| `assets/props.ppm` | Intact/cracked eggs and two lava-troll poses | Selected, point-resized, chroma-key normalized |
| `assets/platform.ppm` | Modular basalt-and-bronze ledge | Selected, cropped, padded, point-resized |
| `assets/stage.ppm` | Moonlit volcanic cavern arena | Selected, gameplay-aligned crop, point-resized |
| `assets/gameover.ppm` | Fallen helmet and broken lance defeat scene | Selected, point-resized |

The generation briefs required original retro pixel art and prohibited copied
game sprites, screenshots, logos, recognizable copyrighted characters,
third-party artwork, text, and watermarks. Every selected image was visually
reviewed and materially prepared through layout selection, cropping, atlas
normalization, chroma-key handling, and binary PPM conversion.

Each rider atlas is a 2-column by 4-row sheet. Frames 1-4 form a generated
four-step ground run, followed by glide, wing-up, wing-down, and dive poses.

## Runtime audio assets

The 40 WAV files under `assets/sfx/` form ten cue-specific variation banks:
menu motion, giant-bird wing flaps and stone footsteps, armored landings and
lance collisions, bird damage, egg settling and hatching, a wave fanfare, and a
molten-lava fall. As of 2026-07-16 every one is **generated locally** by the
`python_sound_assets` generators and is **CC0-derived**; the ElevenLabs bank
they replaced was removed (its blobs remain only in git history).

Each cue is rendered by a specific generator and cue, listed with its exact
command, seed, options, per-file SHA-256, and source recordings in
[`audio-provenance.json`](audio-provenance.json):

- **flap, hurt, egg, hatch** — `creature_foley_generator` (hybrid): layered from
  bundled **CC0** recordings (rubberduck creature/breaking SFX; AntumDeluge's
  Large Wings Flap), each traced in the provenance.
- **step, joust** — `combat_generators` (pure procedural DSP; CC0 by
  construction).
- **land** — `footstep_generator`, a boot on dirt (pure procedural DSP).
- **wave, menu** — `ui_game_state_generator`, arcade style (pure procedural DSP).
- **lava** — `lava_sound_generator` (hybrid): a molten bed from PagDev's
  [Fireplace Sound loop](https://opengameart.org/content/fireplace-sound-loop),
  CC0.

Runtime files are mono 44.1 kHz signed 16-bit PCM WAV, rendered to the game's
exact filenames (base + `_vNN`). The game loads every `_vNN` file into a bank and
avoids immediate repeats. The renders are deterministic: re-running each cue's
recorded command at its recorded seed reproduces the bytes hashed in the
provenance. The C synthesizer remains only as a missing-asset fallback.

Because every runtime SFX is CC0-derived — pure DSP, or CC0/public-domain source
recordings only — the WAVs carry **no standalone-use restriction** and are free
to use, modify, and redistribute. There is no longer any third-party service
term attached to them.

## Runtime inventory policy

Only the eight production PPMs and 40 production WAVs above belong in the
release repository. No generator scripts, prompt logs, raw responses, unused
variants, preview media, converted references, or other pipeline artifacts are
required to build or run the game.
