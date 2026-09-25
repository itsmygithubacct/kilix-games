# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC       ?= cc
PYTHON   ?= python3
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
# In the kilix-games monorepo the SDK is shared; build the kit per game so
# games never share objects built with different flags.
KILIX_GAME_KIT_BUILD_DIR ?= $(CURDIR)/build/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11
override CFLAGS += -ffp-contract=off
LDFLAGS  ?=
LDLIBS   ?= $(KILIX_GAME_KIT_LDLIBS)
PREFIX   ?= /usr/local
DESTDIR  ?=

SRC = src/main.c src/game.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = kilix-pong
SFX_ASSETS := $(sort $(wildcard assets/sfx/*.wav))
EXPECTED_SFX = 21
ASSET_DEST = $(DESTDIR)$(PREFIX)/share/kilix-pong/assets

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/kilix_pong.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/game.o: src/neural_policy_blob.h
src/render.o: src/font8x16.h $(SOFT_RASTER_DIR)/include/soft_raster.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcmmix_bank.h
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

# Headless training/tuning lab for the CPU levels and the neural player.
# Not part of `all`; see tools/neural/README.md.
LAB = tools/neural/pong-lab
lab: $(LAB)
$(LAB): tools/neural/pong_lab.c src/game.o src/kilix_pong.h $(KILIX_GAME_KIT_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tools/neural/pong_lab.c src/game.o \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

sfx:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/gen_sfx.py \
		--out assets/sfx --manifest docs/audio-provenance.json

check-sfx:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/gen_sfx.py --check

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/check-policy.py

check-release-tree:
	@set -eu; \
	bad=$$(find . -path './.git' -prune -o -type f \
		\( -name '*.pyc' -o -name 'render_*.ppm' -o -name 'core' -o -name '*.core' \) \
		-print); \
	[ -z "$$bad" ] || { printf 'non-release files in working tree:\n%s\n' "$$bad" >&2; exit 1; }; \
	[ ! -d tools/__pycache__ ] || { echo 'tools/__pycache__ must not exist in the release tree' >&2; exit 1; }

test: $(BIN) check-sfx check-policy check-release-tree
	./$(BIN) --rules-test
	./$(BIN) --ai-test
	@set -eu; \
	first=$$(mktemp); second=$$(mktemp); \
	trap 'rm -f "$$first" "$$second"' EXIT HUP INT TERM; \
	./$(BIN) --selftest 1337 12000 >"$$first"; \
	./$(BIN) --selftest 1337 12000 >"$$second"; \
	cmp "$$first" "$$second"; \
	cat "$$first"; \
	./$(BIN) --selftest 42 12000
	@set -eu; \
	render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	./$(BIN) --render-test 7 "$$render_dir"; \
	set -- "$$render_dir"/render_*.ppm; \
	[ "$$#" -eq 7 ] || { echo "expected 7 render fixtures, found $$#" >&2; exit 1; }; \
	for image do \
		header=$$(head -n 3 "$$image"); set -- $$header; \
		[ "$$#" -eq 4 ] && [ "$$1" = P6 ] && [ "$$2" = 960 ] && \
			[ "$$3" = 540 ] && [ "$$4" = 255 ] || \
			{ echo "invalid PPM header: $$image" >&2; exit 1; }; \
		header_bytes=$$(printf 'P6\n960 540\n255\n' | wc -c); \
		expected=$$((header_bytes + 960 * 540 * 3)); \
		actual=$$(wc -c < "$$image"); \
		[ "$$actual" -eq "$$expected" ] || \
			{ echo "invalid PPM payload: $$image" >&2; exit 1; }; \
		tail -c +$$((header_bytes + 1)) "$$image" | od -An -tu1 | \
			awk '{ for (i=1; i<=NF; i++) if (!seen[$$i]++) colors++ } \
			     END { exit colors < 16 }' || \
			{ echo "render fixture is effectively blank: $$image" >&2; exit 1; }; \
	done
	@set -eu; \
	install_root=$$(mktemp -d); \
	trap 'rm -rf "$$install_root"' EXIT HUP INT TERM; \
	$(MAKE) --no-print-directory DESTDIR="$$install_root" PREFIX=/usr/local install; \
	(cd / && "$$install_root/usr/local/bin/$(BIN)" --asset-check)

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror -std=c11' \
		LDFLAGS='-fsanitize=address,undefined' $(BIN)
	./$(BIN) --rules-test
	./$(BIN) --ai-test
	./$(BIN) --selftest 1337 12000
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	./$(BIN) --render-test 7 "$$render_dir"
	$(MAKE) clean

install: $(BIN) check-sfx docs/kilix-pong.6
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -Dm644 docs/kilix-pong.6 \
		"$(DESTDIR)$(PREFIX)/share/man/man6/kilix-pong.6"
	install -d -m755 "$(ASSET_DEST)/sfx"
	install -m644 $(SFX_ASSETS) "$(ASSET_DEST)/sfx/"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man6/kilix-pong.6"
	rm -rf "$(DESTDIR)$(PREFIX)/share/kilix-pong"

clean:
	rm -f $(OBJ) $(BIN) $(LAB)

.PHONY: all lab sfx check-sfx check-policy check-release-tree test sanitize install uninstall clean
