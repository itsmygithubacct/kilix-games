# kilix-lights - a standalone graphical Lights Out game for Kitty terminals.

# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
STRIP   ?= strip
TAR     ?= tar
INSTALL ?= install

VERSION ?= 1.0.0
SOURCE_DATE_EPOCH ?= 1784678400
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk

CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDLIBS  += $(KILIX_GAME_KIT_LDLIBS)

RELEASE_CFLAGS := -O2 -DNDEBUG -Wall -Wextra -Wpedantic -std=c11 -pthread \
	-ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=.
SANITIZE_CFLAGS := -O1 -g -Wall -Wextra -Wpedantic -Werror -std=c11 \
	-pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer -ffile-prefix-map=$(CURDIR)=. \
	-fdebug-prefix-map=$(CURDIR)=.

O       := obj
BINDIR  := bin
BIN     := $(BINDIR)/kilix-lights

DIST_PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
DIST_NAME := kilix-lights-$(VERSION)-$(DIST_PLATFORM)
DIST_DIR := dist/$(DIST_NAME)
DIST_ARCHIVE := dist/$(DIST_NAME).tar.gz
DIST_TAR_FLAGS := --sort=name --mtime=@$(SOURCE_DATE_EPOCH) \
	--owner=0 --group=0 --numeric-owner

override CPPFLAGS += -Isrc $(KILIX_GAME_KIT_CPPFLAGS) \
	-I$(KITTY_FRAMEBUFFER_DIR)/src \
	-DKILIX_LIGHTS_VERSION=\"$(VERSION)\" \
	-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
DEPFLAGS := -MMD -MP

SRC := $(sort $(wildcard src/*.c))
OBJ := $(SRC:src/%.c=$(O)/%.o)
DEP := $(OBJ:.o=.d)

.PHONY: all run clean dist-clean test test-code test-render validate-assets \
	check-headers sanitize release-binary dist verify-dist release-check \
	prepare-assets

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

$(O) $(BINDIR):
	mkdir -p $@

$(O)/%.o: src/%.c | $(O)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)/input.o: $(KITTY_INPUT_DIR)/include/kitty_input.h
$(O)/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
$(O)/term.o: $(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h

run: $(BIN)
	./$(BIN)

prepare-assets:
	python3 tools/prepare_assets.py
	python3 tools/generate_lightswitch.py assets/sfx/light-switch.wav

validate-assets: $(BIN)
	python3 tools/validate_assets.py
	./$(BIN) --asset-test

check-headers:
	@set -e; for h in src/*.h; do \
	  $(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only -x c $$h || exit 1; \
	done; echo "check-headers: ok"

test-code: $(BIN)
	./$(BIN) --version
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --interaction-test
	./$(BIN) --sound-test
	@a=$$(./$(BIN) --selftest 1337 500); \
	 b=$$(./$(BIN) --selftest 1337 500); \
	 test "$$a" = "$$b" || { echo "selftest is not deterministic"; exit 1; }; \
	 echo "$$a"

test-render: $(BIN)
	@d=$$(mktemp -d); trap 'rm -rf "$$d"' EXIT; \
	 ./$(BIN) --render-test "$$d"; \
	 python3 tools/validate_renders.py "$$d"

test: test-code check-headers validate-assets test-render

sanitize:
	$(MAKE) clean
	$(MAKE) -C $(KILIX_GAME_KIT_DIR) clean
	$(MAKE) O=obj-san BINDIR=bin-san CFLAGS="$(SANITIZE_CFLAGS)" test
	$(MAKE) -C $(KILIX_GAME_KIT_DIR) clean

release-binary:
	$(MAKE) clean
	$(MAKE) -C $(KILIX_GAME_KIT_DIR) clean
	$(MAKE) CFLAGS="$(RELEASE_CFLAGS)" all
	$(STRIP) -s $(BIN)
	@if strings $(BIN) | grep -Eq '/home/[^/]+|$(CURDIR)'; then \
	  echo "release-binary: absolute build path found" >&2; exit 1; \
	fi
	@echo "release-binary: stripped binary contains no home/build path"

dist: release-binary
	rm -rf $(DIST_DIR) $(DIST_ARCHIVE)
	$(INSTALL) -d $(DIST_DIR)/bin $(DIST_DIR)/assets/art \
		$(DIST_DIR)/assets/sfx $(DIST_DIR)/docs $(DIST_DIR)/licenses
	$(INSTALL) -m 0755 $(BIN) $(DIST_DIR)/bin/kilix-lights
	$(INSTALL) -m 0644 README.md LICENSE THIRD_PARTY.md $(DIST_DIR)/
	$(INSTALL) -m 0644 docs/ASSETS.md $(DIST_DIR)/docs/
	$(INSTALL) -m 0644 assets/art/*.ppm $(DIST_DIR)/assets/art/
	$(INSTALL) -m 0644 assets/sfx/*.wav $(DIST_DIR)/assets/sfx/
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/LICENSE \
		$(DIST_DIR)/licenses/kilix-game-kit.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/kilix-state/LICENSE \
		$(DIST_DIR)/licenses/kilix-state.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/kitty-terminal-session/LICENSE \
		$(DIST_DIR)/licenses/kitty-terminal-session.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/kitty-terminal-session/third_party/kitty-framebuffer/LICENSE \
		$(DIST_DIR)/licenses/kitty-framebuffer.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/kitty-terminal-session/third_party/kitty-input/LICENSE \
		$(DIST_DIR)/licenses/kitty-input.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/kitty-terminal-session/third_party/kitty-input/third_party/kitty_keyboard/LICENSE \
		$(DIST_DIR)/licenses/kitty-keyboard.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/pcm-mixer/LICENSE \
		$(DIST_DIR)/licenses/pcm-mixer.txt
	$(INSTALL) -m 0644 $(KILIX_GAME_KIT_DIR)/third_party/soft-raster/LICENSE \
		$(DIST_DIR)/licenses/soft-raster.txt
	$(TAR) $(DIST_TAR_FLAGS) -C dist -czf $(DIST_ARCHIVE) $(DIST_NAME)
	@echo "dist: wrote $(DIST_ARCHIVE)"

verify-dist: dist
	@d=$$(mktemp -d); trap 'rm -rf "$$d"' EXIT; \
	 $(TAR) -xzf $(DIST_ARCHIVE) -C "$$d"; \
	 "$$d/$(DIST_NAME)/bin/kilix-lights" --version; \
	 "$$d/$(DIST_NAME)/bin/kilix-lights" --asset-test; \
	 test "$$(find "$$d/$(DIST_NAME)/licenses" -type f | wc -l)" -eq 8; \
	 test -r "$$d/$(DIST_NAME)/assets/art/room.ppm"; \
	 test -r "$$d/$(DIST_NAME)/assets/sfx/light-switch.wav"; \
	 echo "verify-dist: archive layout, assets, and 8 license notices are present"

release-check:
	$(MAKE) clean
	$(MAKE) -C $(KILIX_GAME_KIT_DIR) clean
	$(MAKE) all
	$(MAKE) test
	$(MAKE) sanitize
	$(MAKE) clean
	$(MAKE) -C $(KILIX_GAME_KIT_DIR) clean
	$(MAKE) all
	python3 tests/pty_smoke.py --binary ./$(BIN)
	$(MAKE) verify-dist

clean:
	rm -rf $(O) $(BINDIR) obj-san bin-san
	rm -f a-*.d

dist-clean: clean
	rm -rf dist

-include $(DEP)
