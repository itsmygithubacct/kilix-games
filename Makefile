CC       ?= cc
PYTHON   ?= python3
KITTY_FRAMEBUFFER_DIR ?= third_party/kitty-framebuffer
PCM_MIXER_DIR ?= third_party/pcm-mixer
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-I$(KITTY_FRAMEBUFFER_DIR)/include \
	-I$(PCM_MIXER_DIR)/include
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11
override CFLAGS += -ffp-contract=off
LDFLAGS  ?=
LDLIBS   ?= -lz -lm -pthread
PREFIX   ?= /usr/local
DESTDIR  ?=

SRC = src/main.c src/game.c src/render.c src/term.c src/sound.c
VENDOR_OBJ = src/vendor_kitty_framebuffer.o src/vendor_pcm_mixer.o \
	src/vendor_pcm_wav.o
OBJ = $(SRC:.c=.o) $(VENDOR_OBJ)
BIN = kilix-pong
SFX_ASSETS := $(sort $(wildcard assets/sfx/*.wav))
EXPECTED_SFX = 21
ASSET_DEST = $(DESTDIR)$(PREFIX)/share/kilix-pong/assets

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/kilix_pong.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/render.o: src/font8x16.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcm_mixer.h
src/term.o: $(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

src/vendor_kitty_framebuffer.o: $(KITTY_FRAMEBUFFER_DIR)/src/kitty_framebuffer.c \
	$(KITTY_FRAMEBUFFER_DIR)/src/kitty_framebuffer_internal.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/vendor_pcm_mixer.o: $(PCM_MIXER_DIR)/src/pcm_mixer.c \
	$(PCM_MIXER_DIR)/include/pcm_mixer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/vendor_pcm_wav.o: $(PCM_MIXER_DIR)/src/pcm_wav.c \
	$(PCM_MIXER_DIR)/include/pcm_mixer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

sfx:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/gen_sfx.py \
		--out assets/sfx --manifest docs/audio-provenance.json

check-sfx:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/gen_sfx.py --check

check-release-tree:
	@set -eu; \
	bad=$$(find . -path './.git' -prune -o -type f \
		\( -name '*.pyc' -o -name 'render_*.ppm' -o -name 'core' -o -name '*.core' \) \
		-print); \
	[ -z "$$bad" ] || { printf 'non-release files in working tree:\n%s\n' "$$bad" >&2; exit 1; }; \
	[ ! -d tools/__pycache__ ] || { echo 'tools/__pycache__ must not exist in the release tree' >&2; exit 1; }

test: $(BIN) check-sfx check-release-tree
	./$(BIN) --rules-test
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
	[ "$$#" -eq 6 ] || { echo "expected 6 render fixtures, found $$#" >&2; exit 1; }; \
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

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror -std=c11' \
		LDFLAGS='-fsanitize=address,undefined' $(BIN)
	./$(BIN) --rules-test
	./$(BIN) --selftest 1337 12000
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	./$(BIN) --render-test 7 "$$render_dir"

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
	rm -f $(OBJ) $(BIN)

.PHONY: all sfx check-sfx check-release-tree test sanitize install uninstall clean
