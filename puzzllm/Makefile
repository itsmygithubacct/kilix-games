# puzzllm - Kilix in a room with a local coding agent.

.DEFAULT_GOAL := all

CC      ?= cc
VERSION ?= 0.1.0
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
# In the kilix-games monorepo the SDK is shared; build the kit per game so
# games never share objects built with different flags.
KILIX_GAME_KIT_BUILD_DIR ?= $(CURDIR)/build/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk

CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
LDLIBS  += $(KILIX_GAME_KIT_LDLIBS)
override CPPFLAGS += -Isrc $(KILIX_GAME_KIT_CPPFLAGS) \
	-DPUZZLLM_VERSION=\"$(VERSION)\"
DEPFLAGS := -MMD -MP

B        := build
O        := $(B)/obj
CORE     := vt keys pty levels json activity scene art render game kilix_sprite
CORE_OBJ := $(CORE:%=$(O)/%.o)
GAME_OBJ := $(CORE_OBJ) $(O)/termio.o $(O)/main.o
BIN      := $(B)/puzzllm
TEST_BIN := $(B)/test-puzzllm
# Test scratch stays inside the build directory, never in /tmp (a RAM disk).
SCRATCH  := $(CURDIR)/$(B)/test-scratch

.PHONY: all test test-unit test-cli test-sandbox test-defects test-live \
	render clean

all: $(BIN)

$(BIN): $(GAME_OBJ) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(GAME_OBJ) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

$(TEST_BIN): $(CORE_OBJ) $(O)/test_puzzllm.o $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(O)/test_puzzllm.o \
		$(KILIX_GAME_KIT_LIB) $(LDLIBS)

$(O):
	mkdir -p $@

$(O)/%.o: src/%.c | $(O)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)/test_puzzllm.o: tests/test_puzzllm.c | $(O)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

# Unit tests: terminal emulator, keys, levels, JSON, activity feed, scene,
# rendering and the level-1 flow, all headless.
test-unit: $(TEST_BIN)
	@rm -rf $(SCRATCH)/unit && mkdir -p $(SCRATCH)/unit
	PUZZLLM_TEST_DIR=$(SCRATCH)/unit ./$(TEST_BIN)

test-cli: $(BIN)
	@sh tests/test-cli.sh ./$(BIN) $(SCRATCH)/cli

# Confinement: a write outside the workspace fails, the workspace persists,
# the network is closed, the full agent chain works with a fake model.
test-sandbox: $(BIN)
	@bash tests/test-sandbox.sh $(SCRATCH)/sandbox

# Planted defects: each one must turn a test red.
test-defects: $(BIN) $(TEST_BIN)
	@python3 tests/test-defects.py $(SCRATCH)/defects

test: test-unit test-cli test-sandbox test-defects

# Not part of `make test`: needs omp, a llama-server and the Qwen 3.5
# weights on this machine, and runs the real model.
test-live: $(BIN)
	@bash tests/test-live.sh $(SCRATCH)/live

render: $(BIN)
	@mkdir -p $(B)/render && PUZZLLM_ROOT=$(CURDIR) \
		PUZZLLM_STATE=$(SCRATCH)/render-state ./$(BIN) --render $(B)/render

clean:
	rm -rf $(B)

-include $(wildcard $(O)/*.d)
