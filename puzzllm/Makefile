# puzzllm - Kilix in a room with a local coding agent.

.DEFAULT_GOAL := all

CC      ?= cc
VERSION ?= 0.2.0
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
# In the kilix-games monorepo the SDK is shared; build the kit per game so
# games never share objects built with different flags.
KILIX_GAME_KIT_BUILD_DIR ?= $(CURDIR)/build/kilix-game-kit
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk

CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
LDLIBS  += $(KILIX_GAME_KIT_LDLIBS)
LDLIBS  += $(shell pkg-config --libs fontconfig freetype2)
override CPPFLAGS += -Isrc $(KILIX_GAME_KIT_CPPFLAGS) \
	-DPUZZLLM_VERSION=\"$(VERSION)\" $(shell pkg-config --cflags fontconfig freetype2)
DEPFLAGS := -MMD -MP

B        := build
O        := $(B)/obj
CORE     := count_puzzle history keys pty levels workspace json activity leaderboard layout scene draw art render game \
	kilix_art damage kilix_embed yesno yesno_draw
KILIX_ASSETS := third_party/kilix-game-sdk/kilix-assets
NEUROTERM_BUILD_DIR := $(O)/neuroterm
include third_party/neuroterm/embed.mk
CORE_OBJ := $(CORE:%=$(O)/%.o) $(O)/kilix_assets.o $(NEUROTERM_OBJECTS)
override CPPFLAGS += $(NEUROTERM_CPPFLAGS) -I$(KILIX_ASSETS)/include

# The renderer. zell-neuronte's Neuronte renderer is linked when a Neuronte
# checkout (github.com/itsmygithubacct/neuronte) is at NEURONTE. The game
# links that tree's libneuronte and neuronte-sdk's libneuronte-sdk. It does
# not compile neuronte.c or the terminal opener. RENDER=raster builds
# without them; RENDER=neuronte insists on them. At run time
# PUZZLLM_NEURONTE_WEIGHTS names the weight file, and the game draws with
# soft-raster if it does not open.
NEURONTE ?= $(HOME)/projects/neuronte
NEURONTE_SDK ?= $(HOME)/gpu_terminal/kilix-modules/neuronte-sdk
# Split-v9 has a tiled member, so it opens only on a Neuronte that has
# neuronte_set_untrained. An explicit NEURONTE= is kept. The default tree
# is replaced only when it is older and a checkout that contains origin/main
# is visible. The closest such checkout is used.
ifeq ($(origin NEURONTE), file)
ifeq ($(shell grep -q 'neuronte_set_untrained' $(NEURONTE)/include/neuronte.h && echo yes || true),)
DISCOVERED_NEURONTE := $(shell python3 $(NEURONTE_SDK)/scripts/discover_neuronte_src.py 2>/dev/null || true)
ifneq ($(DISCOVERED_NEURONTE),)
NEURONTE := $(DISCOVERED_NEURONTE)
endif
endif
endif
NEURONTE_WEIGHTS ?= $(NEURONTE)/weights/neuronte-split-v4.nrt
# A directory of split-v9, term, and emoji bundles found on this machine.
# Empty when those files are not installed. Not a committed path.
TERMINAL_WEIGHTS_DIR := $(shell python3 $(NEURONTE_SDK)/scripts/discover_terminal_weights.py 2>/dev/null || true)
RENDER ?= auto
HAVE_NEURONTE := $(if $(wildcard $(NEURONTE)/include/neuronte.h),$(if $(wildcard $(NEURONTE)/src/neuronte.c),yes))
ifeq ($(RENDER),neuronte)
ifneq ($(HAVE_NEURONTE),yes)
$(error RENDER=neuronte, but NEURONTE '$(NEURONTE)' has no include/neuronte.h and src/neuronte.c)
endif
endif
ifeq ($(RENDER),raster)
USE_NEURONTE :=
else
USE_NEURONTE := $(HAVE_NEURONTE)
endif
RENDER_IDENT := $(if $(USE_NEURONTE),neuronte $(abspath $(NEURONTE)) $(NEURONTE_WEIGHTS) $(TERMINAL_WEIGHTS_DIR),raster)
ifeq ($(USE_NEURONTE),yes)
ifeq ($(wildcard $(NEURONTE_SDK)/include/neuronte_sdk_terminal.h),)
$(error NEURONTE_SDK '$(NEURONTE_SDK)' has no include/neuronte_sdk_terminal.h)
endif
SDK_BUILD := $(abspath $(B)/neuronte-sdk)
SDK_LIB := $(SDK_BUILD)/libneuronte-sdk.a
NEURONTE_LIB := $(NEURONTE)/build/libneuronte.a
DRAW_CPPFLAGS := -I$(NEURONTE)/include -I$(NEURONTE_SDK)/include -DPUZZLLM_NEURONTE \
	-DPUZZLLM_NEURONTE_WEIGHTS_DEFAULT='"$(NEURONTE_WEIGHTS)"'
DRAW_DEPS := $(NEURONTE_SDK)/include/neuronte_sdk_terminal.h $(SDK_LIB) $(NEURONTE_LIB)
LDLIBS += $(SDK_LIB) $(NEURONTE_LIB) -lm

$(NEURONTE_LIB): $(NEURONTE)/src/neuronte.c $(NEURONTE)/src/utf8.c \
		$(NEURONTE)/src/video.c $(NEURONTE)/src/bits.c $(NEURONTE)/src/cells.c \
		$(NEURONTE)/include/neuronte.h
	$(MAKE) -C $(NEURONTE) build/libneuronte.a

$(SDK_LIB): $(NEURONTE_SDK)/src/nsdk_terminal.c $(NEURONTE_SDK)/src/neuronte_sdk.c \
		$(NEURONTE_SDK)/include/neuronte_sdk_terminal.h $(NEURONTE_SDK)/include/neuronte_sdk.h \
		$(NEURONTE_SDK)/Makefile $(NEURONTE)/include/neuronte.h $(RENDER_STAMP)
	$(MAKE) -C $(NEURONTE_SDK) \
		BUILD_DIR=$(SDK_BUILD) \
		NEURONTE=$(abspath $(NEURONTE)) \
		NEURONTE_INCLUDE=$(abspath $(NEURONTE)/include) \
		NSDK_TERMINAL_EXTRA_DIR='$(TERMINAL_WEIGHTS_DIR)' \
		$(SDK_LIB)
endif
GAME_OBJ := $(CORE_OBJ) $(O)/termio.o $(O)/main.o
BIN      := $(B)/puzzllm
TEST_BIN := $(B)/test-puzzllm
# Test scratch stays inside the build directory, never in /tmp (a RAM disk).
SCRATCH  := $(CURDIR)/$(B)/test-scratch

.PHONY: all test test-unit test-cli test-sandbox test-defects test-live test-kilix-display test-yesno \
	render bench renderer clean

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

$(O)/kilix_assets.o: $(KILIX_ASSETS)/src/kilix_assets.c | $(O)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

# draw.o holds the renderer choice; this file changes when the choice does,
# so switching RENDER or NEURONTE compiles it again.
RENDER_STAMP := $(B)/render.ident
.PHONY: FORCE
FORCE:
$(RENDER_STAMP): FORCE | $(O)
	@printf '%s\n' '$(RENDER_IDENT)' | cmp -s - $@ || printf '%s\n' '$(RENDER_IDENT)' > $@

$(O)/draw.o: src/draw.c $(RENDER_STAMP) $(DRAW_DEPS) | $(O)
	$(CC) $(CPPFLAGS) $(DRAW_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

# A Neuronte build's tests insist that Neuronte draws.
$(O)/test_puzzllm.o: tests/test_puzzllm.c $(RENDER_STAMP) | $(O)
	$(CC) $(CPPFLAGS) $(if $(USE_NEURONTE),-DPUZZLLM_TEST_NEURONTE) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

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

test-kilix-display:
	@python3 -B tests/test_kilix_bridge.py

test-yesno:
	@mkdir -p $(SCRATCH)/yesno
	@TMPDIR=$(SCRATCH)/yesno PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover \
		-s third_party/kilix-bench/tests -p 'test_yesno*.py'

test: test-unit test-cli test-sandbox test-kilix-display test-yesno test-defects

# Not part of `make test`: needs omp, a llama-server and the Qwen 3.5
# weights on this machine, and runs the real model.
test-live: $(BIN)
	@bash tests/test-live.sh $(SCRATCH)/live

render: $(BIN)
	@mkdir -p $(B)/render && PUZZLLM_ROOT=$(CURDIR) \
		PUZZLLM_STATE=$(SCRATCH)/render-state ./$(BIN) --render $(B)/render

# How long a busy frame takes to draw, headless, and which renderer drew it.
bench: $(BIN)
	@PUZZLLM_ROOT=$(CURDIR) PUZZLLM_STATE=$(SCRATCH)/bench-state ./$(BIN) --bench $(BENCH_SIZE)

renderer: $(BIN)
	@./$(BIN) --renderer

clean:
	rm -rf $(B)

-include $(wildcard $(O)/*.d)
