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
override CPPFLAGS += -Isrc $(KILIX_GAME_KIT_CPPFLAGS) \
	-DPUZZLLM_VERSION=\"$(VERSION)\"
DEPFLAGS := -MMD -MP

B        := build
O        := $(B)/obj
CORE     := neuroterm keys pty levels json activity layout scene draw art render game \
	kilix_art damage
KILIX_ASSETS := third_party/kilix-game-sdk/kilix-assets
VTERM := third_party/libvterm
VTERM_SRC := encoding keyboard mouse parser pen screen state unicode vterm
CORE_OBJ := $(CORE:%=$(O)/%.o) $(O)/kilix_assets.o $(VTERM_SRC:%=$(O)/libvterm/%.o)
override CPPFLAGS += -I$(VTERM)/include -I$(KILIX_ASSETS)/include

# The renderer. zell-neuronte's Neuronte renderer is built when a Neuronte
# checkout (github.com/itsmygithubacct/neuronte) is at NEURONTE; its two
# library sources are compiled here. RENDER=raster builds without it;
# RENDER=neuronte insists on it. At run time PUZZLLM_NEURONTE_WEIGHTS names
# the weight file, and the game draws with soft-raster if it does not open.
NEURONTE ?= $(HOME)/projects/neuronte
NEURONTE_WEIGHTS ?= $(NEURONTE)/weights/neuronte-split-v4.nrt
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
RENDER_IDENT := $(if $(USE_NEURONTE),neuronte $(abspath $(NEURONTE)) $(NEURONTE_WEIGHTS),raster)
ifeq ($(USE_NEURONTE),yes)
NEURONTE_OBJ := $(O)/neuronte/neuronte.o $(O)/neuronte/utf8.o
CORE_OBJ += $(NEURONTE_OBJ)
DRAW_CPPFLAGS := -I$(NEURONTE)/include -DPUZZLLM_NEURONTE \
	-DPUZZLLM_NEURONTE_WEIGHTS_DEFAULT='"$(NEURONTE_WEIGHTS)"'
LDLIBS += -lm
endif
GAME_OBJ := $(CORE_OBJ) $(O)/termio.o $(O)/main.o
BIN      := $(B)/puzzllm
TEST_BIN := $(B)/test-puzzllm
# Test scratch stays inside the build directory, never in /tmp (a RAM disk).
SCRATCH  := $(CURDIR)/$(B)/test-scratch

.PHONY: all test test-unit test-cli test-sandbox test-defects test-live \
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

$(O)/libvterm:
	mkdir -p $@

$(O)/libvterm/%.o: $(VTERM)/src/%.c | $(O)/libvterm
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(filter-out -Wall -Wextra -Wpedantic,$(CFLAGS)) -c -o $@ $<

# draw.o holds the renderer choice; this file changes when the choice does,
# so switching RENDER or NEURONTE compiles it again.
RENDER_STAMP := $(B)/render.ident
.PHONY: FORCE
FORCE:
$(RENDER_STAMP): FORCE | $(O)
	@printf '%s\n' '$(RENDER_IDENT)' | cmp -s - $@ || printf '%s\n' '$(RENDER_IDENT)' > $@

$(O)/draw.o: src/draw.c $(RENDER_STAMP) | $(O)
	$(CC) $(CPPFLAGS) $(DRAW_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)/neuronte:
	mkdir -p $@

$(O)/neuronte/%.o: $(NEURONTE)/src/%.c $(NEURONTE)/include/neuronte.h | $(O)/neuronte
	$(CC) -std=c11 -O3 -I$(NEURONTE)/include -c -o $@ $<

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

test: test-unit test-cli test-sandbox test-defects

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
