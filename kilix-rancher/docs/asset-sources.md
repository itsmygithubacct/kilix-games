# Asset provenance and generation record

All project-specific runtime art is original to Kilix Rancher and is distributed
under the repository's MIT License to the extent applicable. The generated
sound effects have the separate bundled-game exception described below. No
assets from *Monster Rancher* or another commercial game are included.

## Runtime visual assets

| Runtime file | Source | Processing |
|---|---|---|
| `assets/kilix.ppm` | Original Kilix identity plate generated with OpenAI's built-in image-generation tool | Flat green source was chroma-keyed with the installed OpenAI image skill helper, visually inspected, trimmed, resized to a fixed 384×384 ground-anchor canvas, and flattened over runtime magenta chroma |
| `assets/backgrounds/ranch.ppm` | Original ranch environment generated with OpenAI's built-in image-generation tool | Center-cropped and normalized to binary 640×360 PPM |
| `assets/backgrounds/arena.ppm` | Original arena environment generated with OpenAI's built-in image-generation tool | Center-cropped and normalized to binary 640×360 PPM |
| `assets/opponents/*.ppm` | Six original rival monsters (Mossnub, Dewdrop, Mistwing, Stonecalf, Moonmoth, Duskcub) generated with the local Gemini image tool (`gemini-3-pro-image`) | Each rendered on a flat chroma-key background, keyed to alpha, trimmed, ground-anchored and scaled into a 320×320 canvas, then flattened over runtime magenta into binary P6 PPM |
| `assets/kilix_atlas.ppm`, `assets/opponents/*_atlas.ppm` | Six-frame animation strips (idle, walk, nap, crouch, pounce, hurt) for the Kilix and every rival | Each pose generated with the creature's base sprite as an `--input` reference (so the character stays consistent), then keyed, trimmed, packed at one shared scale and ground-anchored by `build_atlas.py` into a magenta P6 strip the renderer slices per frame |
| `assets/care/*.ppm` | The four care-basket items (stew, brush, tonic, treat) | Chroma-keyed, trimmed, center-anchored 256×256 magenta P6 item sprites |
| `assets/icons/*.ppm` | Six drill emblem icons (chosen per drill by primary stat) | Chroma-keyed, center-anchored 192×192 magenta P6 emblems; `draw_drill_icon` falls back to procedural art when absent |
| `assets/journal.ppm` | The field-guide book/parchment backdrop | Opaque 640×360 P6 background (no keying); the journal draws its browsable text over it |
| `assets/minigame/*.ppm` | Mini-game props: a signal board, three round countdown numerals, and the flame, bell, and shelter icons the drills act on | Chroma-keyed, trimmed, center-anchored magenta P6 sprites; each drill falls back to procedural shapes when its sprite is absent |
| `assets/font.ppm` | Hand-lettered display font — a 76-glyph monospace atlas (`A-Z a-z 0-9 . , ! ? : ' - / % + & ; ( )`) | Three generated glyph sheets (uppercase, lowercase, digits+punctuation) on flat magenta, sliced into cells by connected components with `font_atlas.py` and packed as GRAYSCALE-on-magenta; the renderer keys the magenta and multiplies each glyph's luminance by the requested text colour, so one atlas serves every UI colour. Used for text at scale ≥ 2; a built-in 5×7 bitmap covers tiny text and any glyph the atlas lacks |
| `assets/rent.ppm` | The monthly rent collector, an original cozy landlord character generated with the local Gemini image tool (`gemini-3-pro-image`) | Rendered on flat green `#00ff00`, chroma-keyed to alpha, trimmed, centre-anchored into a 320×320 canvas and flattened over runtime magenta into binary P6; shown on the rent/eviction event, with a procedural coin-purse fallback when absent |

This repository ships **only runtime assets**. Lossless generation sources,
intermediates, and preprocessing scripts are not part of the distribution.
The tables and prompts in this document are the published provenance record
for the model, chroma-key choice, and renderer contract.

### Rival monster generation

The six rivals share a structured prompt (species, epithet, palette, "cozy
monster-raising" style, three-quarter pose facing slightly left, flat
chroma-key background, no shadow/text/props). Non-green creatures are generated
on green `#00ff00`; the green creature (Mossnub) is generated on magenta
`#ff00ff` instead, because a subject sharing the key colour keys out with the
background. `process_sprite.py` auto-detects the border key colour, so the same
command processes either. The renderer (`draw_opponent` in `src/render.c`) keys
magenta at draw time and ground-anchors each plate like the Kilix. The
validated transparent intermediate is not distributed. Runtime PPM dimensions
are enforced by `make validate-assets`.

### Kilix identity prompt

```text
Use case: stylized-concept
Asset type: game character sprite plate
Primary request: an original fire-kitten monster named Kilix for a cozy monster-raising game; cute but battle-capable, clearly feline, compact athletic body, oversized expressive amber eyes, ember-orange fur, cream muzzle and chest, dark charcoal ear tips and paws, flame-shaped tuft at the end of the tail, tiny glowing ember markings on cheeks and shoulders
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background for background removal
Subject: one single full-body Kilix, fully visible from ear tips to paws and tail, standing in a confident friendly three-quarter hero pose facing slightly right
Style/medium: polished hand-painted 2D game sprite with chunky PS1-era pre-rendered charm, crisp cel-shaded silhouette, simplified fur clumps rather than fine individual hairs, original design
Composition/framing: centered, generous even padding on all sides, character fills about 72 percent of the canvas, no crop
Lighting/mood: warm internal fire glow, cheerful and adventurous, strong readable silhouette
Color palette: ember orange, cream, charcoal, golden yellow, small hot-coral highlights; do not use green anywhere in the subject
Constraints: background must be one perfectly uniform #00ff00 color with no shadows, gradients, texture, reflections, floor plane, or lighting variation; crisp hard outer edges suitable for chroma-key removal; no cast shadow; no contact shadow; no reflection; exactly one character; no props; no text; no logo; no watermark; no border
Avoid: realistic delicate fur strands, photorealism, existing game characters, copyrighted creature designs, extra limbs, duplicate tails, accessories, scenery
```

### Ranch environment prompt

```text
Use case: stylized-concept
Asset type: game environment background
Primary request: an original cozy monster-training ranch built for a fire-kitten creature-raising game
Scene/backdrop: a sunlit highland ranch inside a distant volcanic valley; small round-roof stone barn with an open doorway, low split-rail fences, a straw training circle, water trough, a few ember-red wildflowers, hazy mountains and a peaceful dormant volcano far away
Subject: inviting empty ranch yard with a broad open low-contrast grassy-dirt center where a game character will be composited later
Style/medium: hand-painted 2D game background with late-1990s pre-rendered console charm, chunky shapes, warm painterly pixels, original setting, polished but not photorealistic
Composition/framing: 16:9 wide establishing view, slightly elevated camera, foreground path leading into the broad center, environmental detail concentrated around the edges, no central character or prop blocking the play area
Lighting/mood: soft golden morning light, cheerful, safe, pastoral, faint ember motes near the barn chimney
Color palette: warm sage green, honey gold, terracotta, dusty blue mountains, ember orange accents
Constraints: environment only; broad clear center; no creature; no people; no UI; no text; no signs with writing; no logo; no watermark; no border; no recognizable copyrighted setting or design
Avoid: checkerboard, game board, isometric grid, dark horror mood, busy central detail, modern objects, photorealism
```

### Arena environment prompt

```text
Use case: stylized-concept
Asset type: game battle arena background
Primary request: an original small tournament arena for a fire-kitten monster-raising game
Scene/backdrop: a circular sunken stone arena built into a friendly volcanic highland town; worn sandstone fighting floor, low curved walls, colorful pennants without writing, distant cheering silhouettes, mountain peaks and a glowing caldera on the horizon
Subject: empty battle stage with a very broad low-contrast central floor for two creatures and combat effects to be composited later
Style/medium: hand-painted 2D game background with late-1990s pre-rendered console charm, chunky shapes, warm painterly pixels, original setting, polished but not photorealistic
Composition/framing: 16:9 wide side-on battle view, slightly elevated camera, arena floor spans most of the lower two-thirds, left and right combat positions clear, detail concentrated around edges and upper background
Lighting/mood: late-afternoon festival light, exciting and welcoming rather than brutal, warm rim light
Color palette: sandstone gold, burnt orange, deep teal shadows, faded crimson and cream pennants
Constraints: environment only; empty stage; no creature; no people on the arena floor; no UI; no text; no logos; no watermark; no border; no recognizable copyrighted setting or design
Avoid: colosseum realism, gore, weapons, dark horror mood, busy central detail, modern sports signage, checkerboard, grid, photorealism
```

## Runtime audio assets

The 23 WAV files under `assets/sfx/` form six no-immediate-repeat banks: warm
wooden menu movement and confirmation, a rising training action, padded
creature battle hits, a cheerful victory flourish, and a gentle setback cue.
They were generated specifically for Kilix Rancher with ElevenLabs Text to
Sound Effects v2 (`eleven_text_to_sound_v2`) during the account owner's paid
Starter subscription.

The production field contained 35 candidates. A pinned LAION CLAP model
provided semantic triage, and candidate score arrays were identical across two
independent runs. Signal-shape checks covered onset, event count, tails,
stationary output, and clipping. Selected sources were decoded, onset-aligned,
downmixed with equal power, trimmed or padded to exact runtime length,
DC/high-pass cleaned, edge-faded, and reconstructed-peak gain-staged. No
procedural layer, library sample, third-party recording, or music was mixed
into the masters. Runtime files are mono 44.1 kHz signed 16-bit PCM WAV; all 23
passed automated format, duration, headroom, silence/DC, fade, and duplicate
checks.

ElevenLabs states that qualifying paid-plan output may be used commercially
and indefinitely, while its service-specific policy prohibits standalone
commercial distribution or licensing of Sound Effects output. These WAVs are
therefore excluded from the repository's MIT grant and included only as
bundled Kilix Rancher content, not as a sample pack or sound library. Terms
were checked on 2026-07-14: [paid-plan commercial
use](https://help.elevenlabs.io/hc/en-us/articles/13313564601361-Can-I-publish-the-content-I-generate-on-the-platform),
[Terms of Service](https://elevenlabs.io/terms-of-use), [Sound Effects
Terms](https://elevenlabs.io/sound-effects-terms), and [Prohibited Use
Policy](https://elevenlabs.io/use-policy). Exact prompts, source/final hashes,
selection metrics, and mastering settings are in
[`audio-provenance.json`](audio-provenance.json).

## Code provenance

The Kitty graphics transport and asynchronous presenter in `src/term.c` are
adapted from the MIT-licensed `~/chess-bash` project. Its artwork, chess logic,
music, sound effects, and game-specific renderer were not copied. Kilix
Rancher's game rules, characters, renderer, generated assets, procedural
opponents, effects, UI, and writing are project-specific.
