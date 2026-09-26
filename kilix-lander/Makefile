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

SRC = src/main.c src/game.c src/pilot.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = terminal-lander
SFX_ASSETS := $(sort $(wildcard assets/sfx/*.wav))
EXPECTED_SFX = 21

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/terminal_lander.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/pilot.o: src/neural_policy_blob.h

src/render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

# Headless training/evaluation lab for the neural pilot (not part of `all`);
# see tools/neural/README.md.
LAB = tools/neural/lander-lab
lab: $(LAB)
$(LAB): tools/neural/lander_lab.c src/game.o src/terminal_lander.h $(KILIX_GAME_KIT_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tools/neural/lander_lab.c src/game.o \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

validate-audio:
	@set -eu; \
	count=$$(find assets/sfx -maxdepth 1 -type f -name '*.wav' 2>/dev/null | wc -l); \
	[ "$$count" -eq $(EXPECTED_SFX) ] || \
		{ echo "expected $(EXPECTED_SFX) SFX WAVs, found $$count" >&2; exit 1; }; \
	for sound in assets/sfx/*.wav; do \
		[ "$$(dd if="$$sound" bs=1 count=4 2>/dev/null)" = RIFF ]; \
		[ "$$(dd if="$$sound" bs=1 skip=8 count=4 2>/dev/null)" = WAVE ]; \
	done

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 python3 tools/neural/check-policy.py
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/test-check-policy.py

test: $(BIN) validate-audio check-policy
	./$(BIN) --input-test
	./$(BIN) --menu-test
	./$(BIN) --pilot-test
	./$(BIN) --selftest 1337 3600
	@set -eu; dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT HUP INT TERM; \
		./$(BIN) --render-test 42 "$$dir"; \
		[ "$$(ls "$$dir"/render_*.ppm | wc -l)" -eq 5 ] || { echo "expected 5 render fixtures" >&2; exit 1; }

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -std=c11' \
		LDLIBS='$(KILIX_GAME_KIT_LDLIBS) -fsanitize=address,undefined' $(BIN)
	./$(BIN) --input-test
	./$(BIN) --menu-test
	./$(BIN) --pilot-test
	./$(BIN) --selftest 1337 3600
	@dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT HUP INT TERM; ./$(BIN) --render-test 42 "$$dir" >/dev/null
	$(MAKE) clean

clean:
	rm -f $(OBJ) $(BIN) $(LAB) render_*.ppm render_*.png

.PHONY: all lab check-policy sanitize test validate-audio clean
