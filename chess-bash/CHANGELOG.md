# Changelog

All notable user-facing changes are recorded here.

## Unreleased

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
