# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS  ?=
THREAD_FLAGS ?= -pthread
CHESS_LIBS = $(KILIX_GAME_KIT_LDLIBS)
PREFIX  ?= /usr/local
DESTDIR ?=

SRC = src/main.c src/game.c src/engine.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = chess-bash
ENGINE_LIFECYCLE_TEST = tests/engine-lifecycle

ROOT_IMAGES = assets/title.ppm assets/pieces.ppm assets/pieces_direction.ppm \
	assets/pieces_fight.ppm assets/effects.ppm
BOARD_IMAGES = assets/boards/ivy-courtyard.ppm \
	assets/boards/marble-terminal.ppm assets/boards/desert-ruins.ppm \
	assets/boards/ice-cathedral.ppm assets/boards/volcanic-forge.ppm \
	assets/boards/orbital-space.ppm
SFX_ASSETS = assets/sfx/select.wav assets/sfx/select_v02.wav \
	assets/sfx/select_v03.wav assets/sfx/move_step.wav \
	assets/sfx/move_step_v02.wav assets/sfx/move_step_v03.wav \
	assets/sfx/move_step_v04.wav assets/sfx/move_step_v05.wav \
	assets/sfx/move_step_v06.wav assets/sfx/capture_clank.wav \
	assets/sfx/capture_clank_v02.wav assets/sfx/capture_clank_v03.wav \
	assets/sfx/capture_clank_v04.wav assets/sfx/capture_clank_v05.wav \
	assets/sfx/fall_thud.wav assets/sfx/fall_thud_v02.wav \
	assets/sfx/fall_thud_v03.wav assets/sfx/fall_thud_v04.wav \
	assets/sfx/start_trumpet.wav assets/sfx/start_trumpet_v02.wav \
	assets/sfx/win_trumpet.wav assets/sfx/win_trumpet_v02.wav \
	assets/sfx/check.wav assets/sfx/check_v02.wav assets/sfx/check_v03.wav
MUSIC_ASSETS = assets/music/thinking_loop.wav assets/music/battle_loop.wav \
	assets/music/victory_fanfare.wav
ASSET_FILES = $(ROOT_IMAGES) $(BOARD_IMAGES) $(SFX_ASSETS) $(MUSIC_ASSETS)
ASSET_DEST = $(DESTDIR)$(PREFIX)/share/chess-bash/assets

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) \
		$(THREAD_FLAGS) $(LDLIBS) $(CHESS_LIBS)

src/%.o: src/%.c src/chess_bash.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c -o $@ $<

src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

$(ENGINE_LIFECYCLE_TEST): tests/engine-lifecycle.c src/engine.c src/chess_bash.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -Isrc $(LDFLAGS) \
		-o $@ tests/engine-lifecycle.c src/engine.c $(LDLIBS)

test: $(BIN) $(ENGINE_LIFECYCLE_TEST) validate-assets
	./$(ENGINE_LIFECYCLE_TEST) ./tests/fake-uci.sh
	./$(BIN) --rules-test
	./$(BIN) --perft-test
	./$(BIN) --fallback-test
	./$(BIN) --selftest 1337 160
	./$(BIN) --selftest 42 300
	./$(BIN) --selftest 7 500
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	(cd "$$render_dir" && CHESS_BASH_RENDER_DIR="$$render_dir" \
		"$(CURDIR)/$(BIN)" --render-test 42); \
	set -- "$$render_dir"/render_*.ppm; \
	[ "$$#" -eq 21 ]; \
	for image do [ -s "$$image" ]; done

validate-assets: $(ASSET_FILES)
	@set -eu; \
	check_ppm() { \
		file=$$1; width=$$2; height=$$3; \
		test -f "$$file" || { echo "missing asset: $$file" >&2; return 1; }; \
		header=$$(head -n 3 "$$file"); set -- $$header; \
		[ "$$#" -eq 4 ] && [ "$$1" = P6 ] && [ "$$2" = "$$width" ] && \
			[ "$$3" = "$$height" ] && [ "$$4" = 255 ] || \
			{ echo "invalid PPM header: $$file" >&2; return 1; }; \
		header_bytes=$$(printf 'P6\n%s %s\n255\n' "$$width" "$$height" | wc -c); \
		expected=$$((width * height * 3 + header_bytes)); \
		actual=$$(wc -c < "$$file"); \
		[ "$$actual" -eq "$$expected" ] || \
			{ echo "invalid PPM payload: $$file ($$actual, expected $$expected bytes)" >&2; return 1; }; \
	}; \
	check_wav() { \
		file=$$1; \
		test -s "$$file" || { echo "missing audio asset: $$file" >&2; return 1; }; \
		[ "$$(dd if="$$file" bs=1 count=4 2>/dev/null)" = RIFF ] && \
			[ "$$(dd if="$$file" bs=1 skip=8 count=4 2>/dev/null)" = WAVE ] || \
			{ echo "invalid WAV header: $$file" >&2; return 1; }; \
		format=$$(od -An -tu2 -j20 -N2 "$$file" | tr -d '[:space:]'); \
		channels=$$(od -An -tu2 -j22 -N2 "$$file" | tr -d '[:space:]'); \
		rate=$$(od -An -tu4 -j24 -N4 "$$file" | tr -d '[:space:]'); \
		bits=$$(od -An -tu2 -j34 -N2 "$$file" | tr -d '[:space:]'); \
		[ "$$format" = 1 ] && [ "$$channels" = 1 ] && \
			[ "$$rate" = 44100 ] && [ "$$bits" = 16 ] || \
			{ echo "unsupported WAV format: $$file (need mono 44.1 kHz PCM16)" >&2; return 1; }; \
	}; \
	check_ppm assets/title.ppm 640 360; \
	check_ppm assets/pieces.ppm 768 256; \
	check_ppm assets/pieces_direction.ppm 768 512; \
	check_ppm assets/pieces_fight.ppm 768 1024; \
	check_ppm assets/effects.ppm 640 384; \
	for image in $(BOARD_IMAGES); do check_ppm "$$image" 640 360; done; \
	for audio in $(SFX_ASSETS) $(MUSIC_ASSETS); do check_wav "$$audio"; done

install: $(BIN) validate-assets
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -d -m755 "$(ASSET_DEST)" "$(ASSET_DEST)/boards" \
		"$(ASSET_DEST)/sfx" "$(ASSET_DEST)/music"
	install -m644 $(ROOT_IMAGES) "$(ASSET_DEST)/"
	install -m644 $(BOARD_IMAGES) "$(ASSET_DEST)/boards/"
	install -m644 $(SFX_ASSETS) "$(ASSET_DEST)/sfx/"
	install -m644 $(MUSIC_ASSETS) "$(ASSET_DEST)/music/"
	install -Dm644 docs/chess-bash.6 "$(DESTDIR)$(PREFIX)/share/man/man6/chess-bash.6"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -rf "$(DESTDIR)$(PREFIX)/share/chess-bash"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man6/chess-bash.6"

clean:
	rm -f $(OBJ) $(BIN) $(ENGINE_LIFECYCLE_TEST) \
		render_*.ppm render_*.png battle_*.ppm battle_*.png
	rm -rf .test-render

.PHONY: all test validate-assets install uninstall clean
