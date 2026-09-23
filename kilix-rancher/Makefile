# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS += $(KILIX_GAME_KIT_LDLIBS)
# -pthread must reach BOTH the compile and link steps. It is placed directly on
# the recipes (not tucked into CFLAGS/LDLIBS) so it survives a command-line
# `make CFLAGS=...` override, which would otherwise replace CFLAGS wholesale.

BIN = kilix-rancher
SRC = src/main.c src/game.c src/render.c src/sound.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

PREFIX ?= /usr/local
DESTDIR ?=
ASSET_DEST = $(DESTDIR)$(PREFIX)/share/kilix-rancher/assets
IMAGE_ASSETS = assets/kilix.ppm assets/kilix_atlas.ppm \
	assets/backgrounds/ranch.ppm assets/backgrounds/arena.ppm \
	assets/opponents/mossnub.ppm assets/opponents/dewdrop.ppm \
	assets/opponents/mistwing.ppm assets/opponents/stonecalf.ppm \
	assets/opponents/moonmoth.ppm assets/opponents/duskcub.ppm
SFX_ASSETS := $(sort $(wildcard assets/sfx/*.wav))
EXPECTED_SFX = 23
RUNTIME_ASSETS = $(IMAGE_ASSETS) $(SFX_ASSETS)

all: $(BIN)

# $(CFLAGS) is repeated on the link line so a sanitizer/coverage build
# (make CFLAGS="-fsanitize=address ...") links its runtime correctly.
$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $(OBJ) \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

# -MMD -MP records each object's real header dependencies (in $(DEP)); the
# explicit kilix.h prerequisite covers the very first, pre-.d build.
src/%.o: src/%.c src/kilix.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c -o $@ $<

src/sound.o: $(KILIX_GAME_KIT_DIR)/include/kilix_game_audio.h \
	$(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/game.o: $(KILIX_STATE_DIR)/include/kilix_state.h
src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/main.o: $(KILIX_GAME_KIT_DIR)/include/kilix_game_runtime.h \
	$(KILIX_GAME_KIT_DIR)/include/kilix_game_audio.h \
	$(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h

-include $(DEP)

validate-assets: $(BIN) $(RUNTIME_ASSETS)
	./$(BIN) --validate-assets
	@set -eu; \
	[ "$$(find assets/sfx -maxdepth 1 -type f -name '*.wav' | wc -l)" \
		-eq $(EXPECTED_SFX) ] || \
		{ echo "expected $(EXPECTED_SFX) SFX WAVs" >&2; exit 1; }; \
	for sound in $(SFX_ASSETS); do \
		test "$$(dd if="$$sound" bs=1 count=4 2>/dev/null)" = RIFF; \
		test "$$(dd if="$$sound" bs=1 skip=8 count=4 2>/dev/null)" = WAVE; \
	done

test: $(BIN) validate-assets
	./$(BIN) --selftest 1337 480
	./$(BIN) --selftest 42 240
	@set -eu; \
	render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	./$(BIN) --render-test "$$render_dir" 1337; \
	set -- "$$render_dir"/render_*.ppm; \
	test "$$#" -eq 30; \
	for image do test -s "$$image"; done

audio:
	./tools/generate_audio.sh

install: $(BIN) validate-assets
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -d -m755 "$(ASSET_DEST)" "$(ASSET_DEST)/backgrounds" \
		"$(ASSET_DEST)/opponents" "$(ASSET_DEST)/care" \
		"$(ASSET_DEST)/icons" "$(ASSET_DEST)/minigame" "$(ASSET_DEST)/sfx"
	install -m644 assets/kilix.ppm assets/kilix_atlas.ppm assets/journal.ppm assets/font.ppm assets/rent.ppm \
		"$(ASSET_DEST)/"
	install -m644 assets/backgrounds/*.ppm "$(ASSET_DEST)/backgrounds/"
	install -m644 assets/opponents/*.ppm "$(ASSET_DEST)/opponents/"
	install -m644 assets/care/*.ppm "$(ASSET_DEST)/care/"
	install -m644 assets/icons/*.ppm "$(ASSET_DEST)/icons/"
	install -m644 assets/minigame/*.ppm "$(ASSET_DEST)/minigame/"
	install -m644 $(SFX_ASSETS) "$(ASSET_DEST)/sfx/"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -rf "$(DESTDIR)$(PREFIX)/share/kilix-rancher"

clean:
	rm -f $(OBJ) $(DEP) $(BIN)
	rm -rf .render-test

.PHONY: all validate-assets test audio install uninstall clean
