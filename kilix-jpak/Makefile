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

SRC = src/main.c src/game.c src/data.c src/render.c src/term.c src/sound.c
OBJ = $(SRC:.c=.o)
BIN = kilix-jpak

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/kilix_jpak.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

test: $(BIN) clean-room-check test-cli
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --selftest 1337 12000
	./$(BIN) --selftest 42 6000
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	KILIX_JPAK_RENDER_DIR="$$render_dir" ./$(BIN) --render-test 7; \
	set -- "$$render_dir"/render_*.ppm; \
	[ "$$#" -eq 22 ]; \
	for image do [ -s "$$image" ]; done; \
	! cmp -s "$$render_dir/render_walk_stride_a.ppm" \
	          "$$render_dir/render_walk_stride_b.ppm"

test-fast: $(BIN) test-cli
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --selftest 1337 1800

test-cli: $(BIN)
	@./$(BIN) --version >/dev/null
	@./$(BIN) --dump-level 1 >/dev/null
	@status=0; ./$(BIN) --dump-level 1junk >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]
	@status=0; ./$(BIN) --level +1 >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]
	@status=0; ./$(BIN) --selftest 1 2 extra >/dev/null 2>&1 || status=$$?; \
		[ "$$status" -eq 2 ]

clean-room-check:
	@bad=$$(find . \( -path './.git' -o -path './third_party' \) -prune -o \
		-type f \( -iname '*.dat' -o -iname '*.exe' -o -iname '*.jet' \) -print); \
	[ -z "$$bad" ] || { echo "legacy-format files found:" >&2; echo "$$bad" >&2; exit 1; }
	@! grep -R -E 'JETPACK[0-9]\.DAT|JETLEV\.DAT|JETDEMO\.DAT|JETSOUND\.DAT|SCLM\.DAT|JETPAK15' src

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -Wall -Wextra -Wpedantic -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='-fsanitize=address,undefined'
	./$(BIN) --rules-test
	./$(BIN) --input-test
	./$(BIN) --selftest 9 2400
	@render_dir=$$(mktemp -d); \
	trap 'rm -rf "$$render_dir"' EXIT HUP INT TERM; \
	KILIX_JPAK_RENDER_DIR="$$render_dir" ./$(BIN) --render-test 9
	$(MAKE) clean
	$(MAKE)

install: $(BIN)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -Dm644 docs/kilix-jpak.6 "$(DESTDIR)$(PREFIX)/share/man/man6/kilix-jpak.6"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man6/kilix-jpak.6"

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN) render_*.ppm render_*.png

-include $(OBJ:.o=.d)

.PHONY: all test test-fast test-cli clean-room-check sanitize install uninstall clean
