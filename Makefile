CC      ?= cc
KITTY_TERMINAL_SESSION_DIR ?= third_party/kitty-terminal-session
KITTY_KEYBOARD_DIR ?= $(KITTY_TERMINAL_SESSION_DIR)/third_party/kitty_keyboard
KITTY_FRAMEBUFFER_DIR ?= $(KITTY_TERMINAL_SESSION_DIR)/third_party/kitty-framebuffer
SOFT_RASTER_DIR ?= third_party/soft-raster
PCM_MIXER_DIR ?= third_party/pcm-mixer
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-I$(KITTY_KEYBOARD_DIR)/include \
	-I$(KITTY_FRAMEBUFFER_DIR)/include \
	-I$(KITTY_TERMINAL_SESSION_DIR)/include \
	-I$(SOFT_RASTER_DIR)/include \
	-I$(PCM_MIXER_DIR)/include
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS  ?= -lz -lm -pthread
PREFIX  ?= /usr/local
DESTDIR ?=

SRC = src/main.c src/game.c src/data.c src/render.c src/term.c src/sound.c
VENDOR_OBJ = src/vendor_kitty_terminal_session.o src/vendor_kitty_keyboard.o \
	src/vendor_kitty_keyboard_posix.o src/vendor_kitty_framebuffer.o \
	src/vendor_soft_raster.o src/vendor_pcm_mixer.o src/vendor_pcm_wav.o
OBJ = $(SRC:.c=.o) $(VENDOR_OBJ)
BIN = kilix-jpak

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/kilix_jpak.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_kitty_terminal_session.o: \
	$(KITTY_TERMINAL_SESSION_DIR)/src/kitty_terminal_session.c \
	$(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_kitty_keyboard.o: $(KITTY_KEYBOARD_DIR)/src/kitty_keyboard.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_kitty_keyboard_posix.o: $(KITTY_KEYBOARD_DIR)/src/kitty_keyboard_posix.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_kitty_framebuffer.o: $(KITTY_FRAMEBUFFER_DIR)/src/kitty_framebuffer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_soft_raster.o: $(SOFT_RASTER_DIR)/src/soft_raster.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_pcm_mixer.o: $(PCM_MIXER_DIR)/src/pcm_mixer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

src/vendor_pcm_wav.o: $(PCM_MIXER_DIR)/src/pcm_wav.c
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
