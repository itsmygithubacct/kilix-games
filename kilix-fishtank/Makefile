# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
# In the kilix-games monorepo the SDK is shared; build the kit per game so
# games never share objects built with different flags.
KILIX_GAME_KIT_BUILD_DIR ?= $(CURDIR)/build/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = $(KILIX_GAME_KIT_LDLIBS)

SRC = src/main.c src/game.c src/render.c src/term.c src/audio.c
OBJ = $(SRC:.c=.o)
BIN = kilix-fishtank

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/fishtank.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/render.o: src/embedded_assets.h $(SOFT_RASTER_DIR)/include/soft_raster.h
src/audio.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/term.o: $(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h

test: $(BIN) test-cli
	./$(BIN) --selftest 1337 7200
	./$(BIN) --render-test 42
	./$(BIN) --sound-test

test-cli: $(BIN)
	@set -eu; \
	TMPDIR=$$(mktemp -d); export TMPDIR; trap 'rm -rf "$$TMPDIR"' EXIT; \
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
	check --selftest bad 7200; \
	check --selftest 1x 7200; \
	check --selftest -5 7200; \
	check --selftest -0 7200; \
	check --selftest '' 7200; \
	check --selftest 4294967296 7200; \
	check --selftest 99999999999 7200; \
	check --selftest 1337 0; \
	check --selftest 1337 -1; \
	check --selftest 1337 ''; \
	check --selftest 1337 7200x; \
	check --selftest 1337 10000001; \
	check --selftest 1337 99999999999; \
	check --selftest 1337 7200 extra; \
	accept --selftest 1337 1; \
	accept --selftest 0 1; \
	accept --selftest 4294967295 1; \
	accept --selftest 1337 10000000; \
	check --render-test bad; \
	check --render-test 1x; \
	check --render-test ''; \
	check --render-test 7 extra; \
	check --sound-test extra; \
	check --version extra; \
	check --bogus-option; \
	rm -rf .render-cli

clean:
	rm -f $(OBJ) $(BIN) render_*.ppm render_*.png

.PHONY: all test clean test-cli
