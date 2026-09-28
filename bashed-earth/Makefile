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

SRC = src/main.c src/game.c src/neural.c src/terrain.c src/render.c src/term.c src/config.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = bashed-earth

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/bashed_earth.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/neural.o: src/neural_policy_blob.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

# Headless training/evaluation lab for the neural gunner (not part of `all`);
# see tools/neural/README.md.
LAB = tools/neural/bashed-earth-lab
LAB_OBJ = src/game.o src/neural.o src/terrain.o src/config.o
lab: $(LAB)
$(LAB): tools/neural/bashed_earth_lab.c $(LAB_OBJ) src/bashed_earth.h $(KILIX_GAME_KIT_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tools/neural/bashed_earth_lab.c $(LAB_OBJ) \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/check-policy.py
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/test-check-policy.py

# The test modes keep their options file in a private directory; running
# them under a sentinel HOME and XDG_CONFIG_HOME proves they never write to
# the ones they are given.
test: $(BIN) check-policy
	@sentinel=$$(mktemp -d); trap 'rm -rf "$$sentinel"' EXIT HUP INT TERM; \
	mkdir "$$sentinel/home" "$$sentinel/config"; \
	run() { echo "./$(BIN) $$*"; \
		HOME="$$sentinel/home" XDG_CONFIG_HOME="$$sentinel/config" ./$(BIN) "$$@"; }; \
	run --neural-test && run --selftest 1337 3 && run --selftest 42 2 && \
		run --selftest 7 2 || exit 1; \
	if [ -n "$$(find "$$sentinel/home" "$$sentinel/config" -mindepth 1 -print -quit)" ]; then \
		echo "FAIL: a test mode wrote to HOME or XDG_CONFIG_HOME:" >&2; \
		find "$$sentinel/home" "$$sentinel/config" -mindepth 1 >&2; exit 1; \
	fi; echo "PASS: no test mode touched HOME or XDG_CONFIG_HOME"

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize \
		CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -std=c11' \
		LDLIBS='$(KILIX_GAME_KIT_LDLIBS) -fsanitize=address,undefined' $(BIN)
	./$(BIN) --neural-test
	./$(BIN) --selftest 1337 1
	$(MAKE) clean

clean:
	rm -f $(OBJ) $(BIN) $(LAB)

.PHONY: all test clean lab check-policy sanitize
