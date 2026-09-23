# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS  ?= $(KILIX_GAME_KIT_LDLIBS)
PREFIX  ?= /usr/local
DESTDIR ?=

SRC = src/main.c src/game.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = joustix
ASSET_FILES = assets/stage.ppm assets/gameover.ppm assets/player.ppm assets/bounder.ppm \
	assets/hunter.ppm assets/shadow.ppm assets/props.ppm assets/platform.ppm
SFX_ASSETS := $(sort $(wildcard assets/sfx/*.wav))
EXPECTED_SFX = 40
ASSET_DEST = $(DESTDIR)$(PREFIX)/share/joustix/assets

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/joustix.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/term.o: $(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

test: $(BIN) validate-assets validate-audio
	./$(BIN) --rules-test
	./$(BIN) --selftest 1337 12000
	./$(BIN) --selftest 42 12000
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	JOUSTIX_RENDER_DIR="$$render_dir" ./$(BIN) --render-test 7; \
	set -- "$$render_dir"/render_*.ppm; \
	[ "$$#" -eq 4 ]; \
	for image do [ -s "$$image" ]; done

validate-assets: $(ASSET_FILES)
	@set -eu; \
	[ "$$(find assets -maxdepth 1 -type f | wc -l)" -eq 8 ] || \
		{ echo "assets/ must contain only the eight production images" >&2; exit 1; }; \
	check_ppm() { \
		file=$$1; width=$$2; height=$$3; \
		test -f "$$file" || { echo "missing production image: $$file" >&2; return 1; }; \
		header=$$(head -n 3 "$$file"); set -- $$header; \
		[ "$$#" -eq 4 ] && [ "$$1" = P6 ] && [ "$$2" = "$$width" ] && \
			[ "$$3" = "$$height" ] && [ "$$4" = 255 ] || \
			{ echo "invalid PPM header: $$file" >&2; return 1; }; \
		header_bytes=$$(printf 'P6\n%s %s\n255\n' "$$width" "$$height" | wc -c); \
		expected=$$((width * height * 3 + header_bytes)); \
		actual=$$(wc -c < "$$file"); \
		[ "$$actual" -eq "$$expected" ] || \
			{ echo "invalid PPM payload: $$file ($$actual, expected $$expected)" >&2; return 1; }; \
	}; \
	check_ppm assets/stage.ppm 640 360; \
	check_ppm assets/gameover.ppm 640 360; \
	for image in player bounder hunter shadow; do check_ppm "assets/$$image.ppm" 512 768; done; \
	check_ppm assets/props.ppm 512 512; \
	check_ppm assets/platform.ppm 640 128

validate-audio:
	@set -eu; \
	count=$$(find assets/sfx -maxdepth 1 -type f -name '*.wav' 2>/dev/null | wc -l); \
	[ "$$count" -eq $(EXPECTED_SFX) ] || \
		{ echo "expected $(EXPECTED_SFX) SFX WAVs, found $$count" >&2; exit 1; }; \
	for sound in assets/sfx/*.wav; do \
		[ "$$(dd if="$$sound" bs=1 count=4 2>/dev/null)" = RIFF ]; \
		[ "$$(dd if="$$sound" bs=1 skip=8 count=4 2>/dev/null)" = WAVE ]; \
	done

install: $(BIN) validate-assets validate-audio
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -Dm644 docs/joustix.6 "$(DESTDIR)$(PREFIX)/share/man/man6/joustix.6"
	install -d -m755 "$(ASSET_DEST)" "$(ASSET_DEST)/sfx"
	install -m644 $(ASSET_FILES) "$(ASSET_DEST)/"
	install -m644 $(SFX_ASSETS) "$(ASSET_DEST)/sfx/"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man6/joustix.6"
	rm -rf "$(DESTDIR)$(PREFIX)/share/joustix"

clean:
	rm -f $(OBJ) $(BIN) render_*.ppm

.PHONY: all test validate-assets validate-audio install uninstall clean
