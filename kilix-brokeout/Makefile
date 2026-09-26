# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
PYTHON  ?= python3
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
# In the kilix-games monorepo the SDK is shared; build the kit per game so
# games never share objects built with different flags.
KILIX_GAME_KIT_BUILD_DIR ?= $(CURDIR)/build/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = $(KILIX_GAME_KIT_LDLIBS)

SRC = src/main.c src/game.c src/player.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = kitty-brokeout

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/kitty_brokeout.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/game.o: $(KILIX_STATE_DIR)/include/kilix_state.h
src/player.o: src/neural_policy_blob.h
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

# Headless training/evaluation lab for the neural player (not part of `all`);
# see tools/neural/README.md.
LAB = tools/neural/brokeout-lab
lab: $(LAB)
$(LAB): tools/neural/brokeout_lab.c src/game.o src/kitty_brokeout.h $(KILIX_GAME_KIT_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tools/neural/brokeout_lab.c src/game.o \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 python3 tools/neural/check-policy.py
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/test-check-policy.py

# Every headless mode runs against a sentinel store: XDG data and state point
# at a directory holding a legacy-format high score, and after each mode
# nothing in it (names, modes, sizes, bytes) may have changed. The game must
# isolate itself. --sound-test runs with an empty PATH, so no audio sink is
# found and it stays silent.
test: $(BIN) check-policy
	@set -eu; store=$$(mktemp -d); dir=$$(mktemp -d); \
	trap 'rm -rf "$$store" "$$dir"' EXIT HUP INT TERM; \
	mkdir -p "$$store/kitty-brokeout"; printf '12345\n' > "$$store/kitty-brokeout/highscore"; \
	snapshot() { (cd "$$store" && find . -printf '%p %m %s\n' | sort && sha256sum kitty-brokeout/highscore); }; \
	before=$$(snapshot); export XDG_DATA_HOME="$$store" XDG_STATE_HOME="$$store"; \
	unchanged() { [ "$$before" = "$$(snapshot)" ] || { echo "FAIL: $$1 changed the player's high-score store" >&2; exit 1; }; }; \
	./$(BIN) --input-test; unchanged --input-test; \
	./$(BIN) --menu-test; unchanged --menu-test; \
	./$(BIN) --player-test; unchanged --player-test; \
	./$(BIN) --selftest 1337 7200; unchanged --selftest; \
	./$(BIN) --render-test 42 "$$dir"; unchanged --render-test; \
	[ "$$(ls "$$dir"/render_*.ppm | wc -l)" -eq 6 ] || { echo "expected 6 render fixtures" >&2; exit 1; }; \
	env PATH=/nonexistent ./$(BIN) --sound-test; unchanged --sound-test; \
	echo "PASS: no headless mode touched the high-score store"

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -std=c11' \
		LDLIBS='$(KILIX_GAME_KIT_LDLIBS) -fsanitize=address,undefined' $(BIN)
	./$(BIN) --input-test
	./$(BIN) --menu-test
	./$(BIN) --player-test
	./$(BIN) --selftest 1337 7200
	@dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT HUP INT TERM; ./$(BIN) --render-test 42 "$$dir" >/dev/null
	$(MAKE) clean

clean:
	rm -f $(OBJ) $(BIN) $(LAB) render_*.ppm render_*.png

.PHONY: all lab check-policy test sanitize clean
