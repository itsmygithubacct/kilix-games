# Asset provenance and licensing

This file records the origin and redistribution terms of every non-code asset
shipped with Chess Bash. The runtime files in `assets/` are the canonical
release assets; private workstations, unpublished archives, and external source
trees are not required to build or run the game.

Except for the embedded font, the project-specific visual assets and
the music are distributed under the repository's MIT License, and the
locally-generated CC0-derived sound effects described below carry no restriction
at all, to the extent copyright or
related rights exist. They were created specifically for Chess Bash and do not
incorporate source files from another chess game.

## Visual assets

| Runtime files | Origin | Release processing |
|---|---|---|
| `assets/title.ppm` | Original pixel-art key art generated with xAI Grok image generation (2026-11-23) | Scaled to 960x540 binary PPM |
| `assets/pieces.ppm` | Original chess-army sprite concept generated with Google Gemini | Chroma-keyed runtime atlas |
| `assets/pieces_direction.ppm` | Gemini-generated directional study derived from the project sprite concept | Normalized directional atlas |
| `assets/pieces_fight.ppm` | Gemini-generated battle poses using the project sprites as character references | Identity-checked and normalized fight atlas |
| `assets/effects.ppm` | Original effects study generated with Google Gemini | Chroma-keyed runtime atlas |
| `assets/boards/<theme>.ppm` | Original Gemini environment concepts, edited with OpenAI image generation | Boards and checker grids removed; center-cleared and normalized to 640x360 PPM |
| `assets/boards/<theme>-light.ppm`, `-dark.ppm` | Original stone, ice and metal surface textures generated with xAI Grok image generation (2026-11-23), one prompt per texture | Border cropped, scaled to 256x256 binary PPM; brightness is levelled at load time |

The image-generation briefs required original retro pixel art and prohibited
copied game sprites, screenshots, logos, recognizable copyrighted characters,
and third-party artwork. The background edit brief additionally required a
boardless environment with an open, low-contrast center because the game draws
its own board there.

All generated images were visually reviewed and materially prepared for the
game through selection, layout decisions, cropping, atlas construction,
chroma-key cleanup, and runtime conversion. Google, OpenAI and xAI are tool
providers, not authors, sponsors, or endorsers of Chess Bash.

## Audio assets

The 25 WAV files under `assets/sfx/` form seven cue-specific variation banks:
selection taps, footsteps, sword contacts, body falls, start and win trumpets,
and a check-warning alert. As of 2026-07-16 every one is **generated locally**
by the `python_sound_assets` generators and is **CC0-derived**; the ElevenLabs
bank they replaced was removed (its blobs remain only in git history).

Each cue is rendered by a specific generator, listed with its exact command,
seed, per-file SHA-256, and source recordings in
[`audio-provenance.json`](audio-provenance.json):

- **move_step, capture_clank, fall_thud** — `combat_generators` `chess-bash`
  pack (`footstep`, `strike` sword-on-blade, `body_fall`): pure procedural DSP,
  CC0 by construction.
- **select, check** — `ui_game_state_generator`, acoustic style
  (`menu_accept`, `warning`): pure procedural DSP.
- **start_trumpet, win_trumpet** — `music_sound_generator`, short Strudel
  fanfares (`start_fanfare`, `win_fanfare`): synthesised waveforms only, no
  sample banks. These are real-time captures and so not byte-deterministic; the
  reproducible pin is the Strudel pattern text (`pattern_sha256`), and the
  captured takes were normalised to −0.5 dBFS.

Runtime files are mono 44.1 kHz signed 16-bit PCM WAV, rendered to the game's
exact filenames (base + `_vNN`), and the game avoids immediate repeats within a
bank. The procedural cues are deterministic: re-running each recorded command at
its seed reproduces the hashed bytes.

Because every runtime SFX is CC0-derived — pure DSP or synthesised Strudel
patterns, no sample banks — the WAVs carry **no standalone-use restriction** and
are free to use, modify, and redistribute. There is no longer any third-party
service term attached to them. Per-file
hashes, commands, and pattern pins are recorded in
[`audio-provenance.json`](audio-provenance.json).

The game also contains a C-based procedural fallback for
sound effects when runtime assets cannot be loaded.

## Music

The four WAV files under `assets/music/` are **AI-generated**. They were made
on 2026-11-24 with the openly released
[MiniMax-Music3](https://huggingface.co/MiniMaxAI/MiniMax-Music3) text-to-music
model, run locally from the published weights; no hosted service was used, so
no service terms attach to them. Each track came from a written description of
an instrumental piece (style, tempo, key, instruments) and a seed.

| File | Use | Made from |
| --- | --- | --- |
| `battle_loop.wav` | title screen, loop | an orchestral march, 25.6 s cut |
| `thinking_loop.wav` | in game, loop | a lute and strings piece, 21.8 s cut |
| `thinking_loop_b.wav` | in game on alternate games, loop | a recorder, viola and harp piece, 43.5 s cut |
| `victory_fanfare.wav` | after a win, played once | a brass fanfare, first 10.8 s |

After generation each take was mixed to mono, filtered below 25 Hz and above
16 kHz, cut at bar lines, crossfaded over two or three seconds so the loop has
no step at the join, and levelled with a limiter to a peak of -1 dBFS. The
descriptions, seeds, cut points and file hashes are in the `music` block of
[`audio-provenance.json`](audio-provenance.json). The model does not return
the same samples on every run, so the hashes identify the shipped files; they
are not a way to regenerate them.

The model is published under the MiniMax-Music3 Community License. It places
no restriction on redistributing output, and asks that machine-generated
content be disclosed as such, which this section and the `LICENSE` file do,
and that commercial products built on the model display the name
"MiniMax-Music3". The tracks are provided as is. The earlier music, three
locally synthesised Strudel patterns, remains in the git history.

## Embedded font

`third_party/soft-raster/src/font8x16.h` contains glyphs extracted from Debian console-setup's
`Lat15-Fixed16` console font. Debian describes its console fonts as public
domain. The font's X11-lineage permissive notice, including the Sony Corp.,
BIZNET Poland, and Dmitry Yu. Bolkhovityanov notices, is preserved verbatim in
the header and summarized in the root `LICENSE` file.

## Runtime inventory

The release payload contains:

```text
assets/title.ppm
assets/pieces.ppm
assets/pieces_direction.ppm
assets/pieces_fight.ppm
assets/effects.ppm
assets/boards/*.ppm
assets/sfx/*.wav
assets/music/*.wav
```

Historical experiments, intermediate generations, raw API responses, unused
atlases, and alternate media encodings are intentionally excluded from the
public runtime repository.
