# Changelog

All notable user-facing changes are recorded here.

## Unreleased

### Added

- A top-down view beside the isometric one: T or Tab switches at any time, and
  the title menu has a View row
- Textured stone boards with a frame for every battlefield, new title art
- Moves are shown in algebraic notation; the top-down view has a score sheet
- Distinct sound cues for castling, promotion, illegal moves, draws, defeat and
  the menus, and an optional local audio pack under `assets/pack/` that can
  replace any bundled sound or track
- H shows a hint from the built-in search
- The menu choices (mode, side, difficulty, battlefield, view, music, battle
  speed) are remembered between runs
- Take back a move with U or Backspace, including after checkmate or a draw;
  against the computer its reply goes back too
- After the game, V hides the result banner so the final position can be seen

### Fixed

- The board squares were coloured the wrong way round (a1 was light)
- Losing to the computer no longer plays the victory fanfare
- A board frame cost about twice its 30 Hz budget at 1000x640, and game time
  advanced per frame, so play ran in slow motion; a frame now costs about an
  eighth as much and game time follows the clock
- Skipping a battle played every remaining sound cue at once
- Holding R or N no longer counts as the second press that resigns or leaves
- The title menu and battle caption no longer cover the title or the board in
  80x24 and smaller terminals
- Missing assets are reported before the terminal is taken over, instead of
  starting a game with an empty board
- `--perft` rejects a FEN whose en-passant square cannot be real

### Changed

- Replaced every gameplay SFX cue with locally-generated CC0-derived audio,
  removing the ElevenLabs bank: procedural footsteps, sword contacts and falls
  (`combat_generators`), selection and check cues (`ui_game_state_generator`),
  and start/win trumpets composed as Strudel synth fanfares
  (`music_sound_generator`). The three music tracks were already local. The SFX
  now carry no standalone-use restriction; see `docs/asset-sources.md`.
- Reauthored every gameplay sound cue as a mastered 25-file ElevenLabs
  variation bank with recognizable footsteps, sword contacts, armored falls,
  trumpets, bell warnings, and selection taps
- Added shipped check-warning audio instead of relying only on the fallback synth
- Avoided immediate sound repeats and strengthened WAV format validation

### Documentation

- Added artifact-level audio generation, selection, mastering, paid-plan terms,
  and license provenance

## 0.2.0 - 2026-07-12

First public release.

### Added

- Human, computer, and computer-vs-computer play with three difficulty levels
- Complete draw, castling, en-passant, and promotion handling
- Six boardless battlefield environments with a centered isometric board
- Directional movement, multi-stage capture battles, music, sound, and finale
- Blue-side board rotation, compact-terminal feedback, move markers, and check cues
- Deterministic rule, perft, engine, render, asset, and installation tests

### Changed

- Improved fallback AI responsiveness and Stockfish lifecycle handling
- Made abandon and resignation actions require confirmation
- Hardened terminal presentation, resize behavior, audio latency, and cleanup
- Prepared reproducible, permission-safe installation and public CI coverage
