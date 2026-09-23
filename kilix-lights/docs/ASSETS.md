# Asset provenance

All game-specific art and audio in Kilix Lights was created for this project on 2026-07-21. No external game artwork or recorded sound was ingested.

## Electrical workshop

- Source: `assets/source/electrical-workshop.png`
- Runtime derivative: `assets/art/room.ppm`, resized/cropped to 640×400 by `tools/prepare_assets.py`
- Generation mode: built-in `image_gen`, text-to-image, with no reference images
- Prompt:

> Use case: stylized-concept art for a standalone desktop game. Create an original full-color environment background for a polished “Lights Out” puzzle game. A cozy retro electrical workshop and control room viewed straight on, landscape 16:10 composition. Warm walnut wood, aged brass trim, dark teal painted metal, cream ceramic insulators, subtle cables and shelves around the perimeter, a few unlit practical lamps, and a believable workbench-room atmosphere. Reserve a large, clean, empty, nearly rectangular dark enamel wall/control panel across the left-center roughly 58% of the image width for a code-rendered 5x5 puzzle board. Reserve a quieter narrow area on the right for code-rendered status and buttons. Keep both reserved areas visually uncluttered. The central panel must be front-facing with minimal perspective distortion and no physical switches, lamps, labels, writing, icons, or game tiles already on it. Moody but readable warm amber workshop practical light from above and cool blue-green ambient fill, rich materials, gentle vignette, strong visual hierarchy, legible when reduced to 640x400. Polished hand-painted game environment with tactile slightly 3D materials, charming and grounded rather than cartoon-flat, crisp enough for a small terminal-window game. No people, no text, no numbers, no logos, no watermark, no interface, no border, no existing game assets.

## Vintage switch

- Chroma-key source: `assets/source/vintage-switch-green.png`
- Transparent source: `assets/source/vintage-switch-alpha.png`
- Runtime derivatives: `assets/art/switch.ppm` and `assets/art/switch-mask.ppm`, 96×96
- Generation mode: built-in `image_gen`, text-to-image, with no reference images
- Chroma removal: the image-generation skill's `remove_chroma_key.py`, using border auto-key, soft matte, thresholds 12/220, and despill
- Prompt:

> Create one original isolated graphical asset for a polished retro electrical puzzle game. A tactile vintage wall light switch assembly viewed perfectly straight-on. A compact nearly-square aged brass and dark teal enamel mounting plate, four small corner screws, cream ceramic center, and a chunky dark Bakelite toggle lever in a neutral/up position. Symmetrical, iconic silhouette, crisp material details, readable when reduced to about 64 pixels, charming hand-painted slightly 3D game-art style. Warm highlights and subtle wear, but no cast shadow outside the object. The single complete switch centered with generous even padding, entirely visible, no cropped edges, orthographic/front view. Background must be a perfectly flat, uniform pure chroma green #00FF00 from edge to edge, with no gradient, texture, shadow, green reflections, or extra objects. No text, no numbers, no logos, no watermark, no border, no hands, no wall, no room, no duplicate sprites.

The runtime mask doubles as the semantic hit-test mask. Code applies state tint, light bloom, a distinct lever position, hover halo, and keyboard-focus halo while preserving the generated plate artwork.

## Light-switch cue

- Runtime file: `assets/sfx/light-switch.wav`
- Generator: `tools/generate_lightswitch.py`
- Format: 44,100 Hz, mono, signed 16-bit PCM, 6,836 frames (0.155 seconds)
- Construction: deterministic damped low-body snap, bright noise transient, secondary clack, and short settling resonance; normalized with headroom and explicit fades
- SHA-256: `b0c45aa71395c307c7f5b24d7d52412fe40074a0f1d11af88c903c16003c02bc`

Run `python3 tools/validate_assets.py` to verify dimensions, mask semantics, waveform format, non-silence, and current hashes.
