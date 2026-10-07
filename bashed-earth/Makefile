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
test: $(BIN) $(LAB) check-policy test-cli
	@sentinel=$$(mktemp -d); trap 'rm -rf "$$sentinel"' EXIT HUP INT TERM; \
	mkdir "$$sentinel/home" "$$sentinel/config"; \
	run() { echo "./$(BIN) $$*"; \
		HOME="$$sentinel/home" XDG_CONFIG_HOME="$$sentinel/config" ./$(BIN) "$$@"; }; \
	run --menu-test && run --neural-test && \
		echo "$(LAB) --self-test" && $(LAB) --self-test && run --selftest 1337 3 && run --selftest 42 2 && \
		run --selftest 7 2 || exit 1; \
	if [ -n "$$(find "$$sentinel/home" "$$sentinel/config" -mindepth 1 -print -quit)" ]; then \
		echo "FAIL: a test mode wrote to HOME or XDG_CONFIG_HOME:" >&2; \
		find "$$sentinel/home" "$$sentinel/config" -mindepth 1 >&2; exit 1; \
	fi; echo "PASS: no test mode touched HOME or XDG_CONFIG_HOME"

test-cli: $(BIN)
	@set -eu; \
	TMPDIR=$$(mktemp -d); export TMPDIR; trap 'rm -rf "$$TMPDIR"' EXIT HUP INT TERM; \
	check() { \
		set +e; timeout 10 ./$(BIN) "$$@" >/dev/null 2>&1; rc=$$?; set -e; \
		if [ "$$rc" -ne 2 ]; then \
			echo "test-cli: expected exit 2, got $$rc: $(BIN) $$*" >&2; exit 1; \
		fi; \
	}; \
	accept() { \
		set +e; timeout 2 ./$(BIN) "$$@" >/dev/null 2>&1; rc=$$?; set -e; \
		if [ "$$rc" -eq 2 ]; then \
			echo "test-cli: valid arguments refused: $(BIN) $$*" >&2; exit 1; \
		fi; \
	}; \
	check --selftest bad 1; \
	check --selftest 1x 1; \
	check --selftest -5 1; \
	check --selftest -0 1; \
	check --selftest '' 1; \
	check --selftest 4294967296 1; \
	check --selftest 99999999999 1; \
	check --selftest 1337 0; \
	check --selftest 1337 -1; \
	check --selftest 1337 ''; \
	check --selftest 1337 1x; \
	check --selftest 1337 1001; \
	check --selftest 1337 99999999999; \
	check --selftest 1337 1 extra; \
	accept --selftest 1337 1; \
	accept --selftest 0 1; \
	accept --selftest 4294967295 1; \
	accept --selftest 1337 1000; \
	check --render-test bad; \
	check --render-test 1x; \
	check --render-test ''; \
	check --render-test 7 extra; \
	check --version extra; \
	check --bogus-option; \
	rm -rf .render-cli

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize \
		CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -std=c11' \
		LDLIBS='$(KILIX_GAME_KIT_LDLIBS) -fsanitize=address,undefined' $(BIN) $(LAB)
	$(LAB) --self-test
	./$(BIN) --menu-test
	./$(BIN) --neural-test
	./$(BIN) --selftest 1337 1
	$(MAKE) clean

clean:
	rm -f $(OBJ) $(BIN) $(LAB)

.PHONY: all test clean lab check-policy sanitize test-cli
