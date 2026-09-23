# Tests

Kilix Rancher's test suite is built into the game binary and driven by `make test`:

- `./kilix-rancher --selftest [seed] [weeks]` — deterministic simulation
  checks: identical seeds reproduce identical growth, calendar/season
  boundaries are exact, battles settle exactly once, the arena Ready prompt
  cancels without penalty, corrupt/truncated saves are rejected without
  partially mutating state, and version-2 (pre-economy) saves migrate forward
  with new-ranch defaults.
- `./kilix-rancher --render-test [dir] [seed]` — renders 30 headless snapshots
  of every screen to PPM files and checks they are all present and non-empty.
- `./kilix-rancher --validate-assets` — verifies the bundled images and WAVs.

Run them all at once:

```sh
make test
```

This directory holds no separate test files; the harness lives in
`src/game.c` (`game_selftest`) and `src/main.c` (`render_test`).
