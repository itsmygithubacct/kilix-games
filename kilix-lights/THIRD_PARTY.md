# Third-party software

Kilix Lights links statically against the recursively pinned `kilix-game-kit` submodule. Gitlinks in that repository pin every transitive dependency, so a recursive checkout builds the same reviewed source versions.

| Component | License file |
|---|---|
| kilix-game-kit | `third_party/kilix-game-kit/LICENSE` |
| kilix-state | `third_party/kilix-game-kit/third_party/kilix-state/LICENSE` |
| kitty-terminal-session | `third_party/kilix-game-kit/third_party/kitty-terminal-session/LICENSE` |
| kitty-framebuffer | `third_party/kilix-game-kit/third_party/kitty-terminal-session/third_party/kitty-framebuffer/LICENSE` |
| kitty-input | `third_party/kilix-game-kit/third_party/kitty-terminal-session/third_party/kitty-input/LICENSE` |
| kitty-keyboard | `third_party/kilix-game-kit/third_party/kitty-terminal-session/third_party/kitty-input/third_party/kitty_keyboard/LICENSE` |
| pcm-mixer | `third_party/kilix-game-kit/third_party/pcm-mixer/LICENSE` |
| soft-raster | `third_party/kilix-game-kit/third_party/soft-raster/LICENSE` |

All are MIT licensed. Source distributions retain each notice at the paths above. Binary archives produced by `make dist` copy all eight notices into their `licenses/` directory.
