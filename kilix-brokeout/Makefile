# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS)
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = $(KILIX_GAME_KIT_LDLIBS)

SRC = src/main.c src/game.c src/render.c src/term.c src/sound.c
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
src/term.o: $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
	$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

test: $(BIN)
	./$(BIN) --input-test
	./$(BIN) --selftest 1337 7200
	./$(BIN) --render-test 42

clean:
	rm -f $(OBJ) $(BIN) render_*.ppm render_*.png

.PHONY: all test clean
