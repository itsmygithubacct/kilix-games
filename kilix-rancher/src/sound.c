#include "kilix.h"
#include "kilix_game_audio.h"

/* The 23 bundled variations across the six cues. Every cue is optional: with
 * assets missing or no usable mixer the game stays fully playable, silent.
 * Variant selection (random, no immediate repeat) comes from the shared cue
 * bank. */
#define SFX(id, variant, file) {(uint32_t)(id), (variant), (file), \
                                1.0f, 1.0f, false}

static const kilix_game_audio_cue_spec SFX_SPECS[] = {
    SFX(SFX_MOVE, 0u, "sfx/move.wav"),
    SFX(SFX_MOVE, 1u, "sfx/move_v02.wav"),
    SFX(SFX_MOVE, 2u, "sfx/move_v03.wav"),
    SFX(SFX_MOVE, 3u, "sfx/move_v04.wav"),
    SFX(SFX_CONFIRM, 0u, "sfx/confirm.wav"),
    SFX(SFX_CONFIRM, 1u, "sfx/confirm_v02.wav"),
    SFX(SFX_CONFIRM, 2u, "sfx/confirm_v03.wav"),
    SFX(SFX_TRAIN, 0u, "sfx/train.wav"),
    SFX(SFX_TRAIN, 1u, "sfx/train_v02.wav"),
    SFX(SFX_TRAIN, 2u, "sfx/train_v03.wav"),
    SFX(SFX_TRAIN, 3u, "sfx/train_v04.wav"),
    SFX(SFX_HIT, 0u, "sfx/hit.wav"),
    SFX(SFX_HIT, 1u, "sfx/hit_v02.wav"),
    SFX(SFX_HIT, 2u, "sfx/hit_v03.wav"),
    SFX(SFX_HIT, 3u, "sfx/hit_v04.wav"),
    SFX(SFX_HIT, 4u, "sfx/hit_v05.wav"),
    SFX(SFX_HIT, 5u, "sfx/hit_v06.wav"),
    SFX(SFX_WIN, 0u, "sfx/win.wav"),
    SFX(SFX_WIN, 1u, "sfx/win_v02.wav"),
    SFX(SFX_WIN, 2u, "sfx/win_v03.wav"),
    SFX(SFX_LOSE, 0u, "sfx/lose.wav"),
    SFX(SFX_LOSE, 1u, "sfx/lose_v02.wav"),
    SFX(SFX_LOSE, 2u, "sfx/lose_v03.wav"),
};

static kilix_game_audio audio;

bool sound_init(void)
{
    kilix_game_audio_options options;
    char error[256];

    if (G.headless) return false;
    kilix_game_audio_options_init(&options);
    options.cue_count = SFX_COUNT;
    options.random_seed = 0x4b17c4e5u;
    /* asset_paths_init already located the asset root; resolve cue paths
     * beneath it rather than re-deriving roots here. */
    options.data.local_root = asset_path("");
    options.cues = SFX_SPECS;
    options.cue_spec_count = sizeof SFX_SPECS / sizeof SFX_SPECS[0];
    if (!kilix_game_audio_init(&audio, &options, error, sizeof error))
        return false;
    return kilix_game_audio_is_running(&audio);
}

void sound_play(SoundId id)
{
    if (G.headless || !G.sound_on || (int)id < 0 || id >= SFX_COUNT)
        return;
    (void)kilix_game_audio_play(&audio, (uint32_t)id,
                                KILIX_GAME_AUDIO_BUS_SFX, 1.0f, 1.0f);
}

void sound_shutdown(void)
{
    kilix_game_audio_shutdown(&audio);
}
