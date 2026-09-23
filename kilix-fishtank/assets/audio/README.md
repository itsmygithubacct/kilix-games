# Fishtank Audio Assets

`fishtank_ambience.wav` is the default quiet aquarium loop used by
`kilix-fishtank` in interactive mode. It is a seamless 45-second loop: an
intermittent bubbler over faint water. `fishtank_ambience.webm` is retained as a
compact reference copy, but the runtime only loads mono 44.1 kHz PCM16 WAV.

The `events/` directory contains short cues for feeding, food bites, shark
bites, frenzy, landed catches, lure bites, successful hook sets, escaped fish,
snapped lines, and shark arrivals. These are edge-triggered game events rather
than continuous state sounds: each cue plays once when its named transition
occurs. They play through the same in-process mixer, so the game does not
launch an external media player.

| File | Edge-triggered meaning |
| --- | --- |
| `feed_splash.wav` | feed enters the water |
| `fish_bite.wav` | a fish consumes feed |
| `shark_bite.wav` | a shark consumes a fish |
| `frenzy.wav` | the frenzy meter reaches its threshold |
| `catch_splash.wav` | the player lands a hooked fish |
| `hook_bite.wav` | a fish first bites the lure |
| `hook_set.wav` | the player reels during the bite window and sets the hook |
| `fish_escape.wav` | a bite window expires or a hooked fish escapes without a snap |
| `line_snap.wav` | excessive line tension snaps the line |
| `shark_alert.wav` | a new shark enters the tank |

## Provenance

The ambience and water event cues are derived from Creative Commons Zero (CC0)
source recordings, layered and rendered by deterministic procedural generators.
The bite and frenzy cues are generated procedurally. No rights-restricted audio
is used.

- **Source:** rubberduck, *40 CC0 water / splash / slime SFX* —
  <https://opengameart.org/content/40-cc0-water-splash-slime-sfx>
- **Licence:** CC0 1.0 (public domain dedication), read from the source page.
- **Layers:** bubble recordings from that pack, placed in intermittent bursts,
  over a faint low-passed water bed from the same pack. The loop is seamless by
  construction — bursts that reach the end wrap to the start.

Because the source is CC0, this file carries no attribution obligation; the
credit above is recorded for provenance rather than as a licence requirement.

Exact generator settings and output hashes are recorded in
`provenance/water-events.json`, `provenance/creature-events.json`, and
`provenance/ui-events.json`.

## Playback

```sh
./kilix-fishtank --sound-test
```

## Runtime controls

```sh
KILIX_FISHTANK_NO_AUDIO=1 ./kilix-fishtank
KILIX_FISHTANK_AUDIO_VOLUME=12 ./kilix-fishtank
KILIX_FISHTANK_AUDIO=/path/to/mono-44100-pcm16-loop.wav ./kilix-fishtank
```
