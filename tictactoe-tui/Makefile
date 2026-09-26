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

SRC = src/main.c src/engine.c src/game.c src/render.c src/term.c
OBJ = $(SRC:.c=.o)
BIN = tictactoe-tui

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

src/%.o: src/%.c src/ttt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/engine.o: src/neural_policy_blob.h

# Retrains the network (numpy) and reinstalls the blob, header and provenance.
train:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/train.py

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/check-policy.py
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/test-check-policy.py

test: $(BIN) check-policy
	./$(BIN) --rules-test
	./$(BIN) --neural-test
	./$(BIN) --render-test
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tests/pty_check.py
	@set -eu; \
	root=$$(mktemp -d); trap 'rm -rf "$$root"' EXIT HUP INT TERM; \
	$(MAKE) --no-print-directory DESTDIR="$$root" PREFIX=/usr/local install; \
	"$$root/usr/local/bin/$(BIN)" --version

# The instrumented build uses its own SDK build directory, so the normal
# SDK archive never holds sanitizer objects and a later plain make links.
sanitize:
	$(MAKE) clean
	$(MAKE) KILIX_GAME_KIT_BUILD_DIR=$(CURDIR)/build/kilix-game-kit-sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror -std=c11' \
		LDFLAGS='-fsanitize=address,undefined' $(BIN)
	./$(BIN) --rules-test
	./$(BIN) --neural-test
	./$(BIN) --render-test
	$(MAKE) clean

install: $(BIN)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"

clean:
	rm -f $(OBJ) $(BIN)
	rm -rf build

.PHONY: all train check-policy test sanitize install uninstall clean
