# Test Makefile for Astonia Client
# Compiles and runs unit tests without external test frameworks

# Detect platform
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
else
    PLATFORM := windows
endif

CC = clang
CFLAGS = -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-unused-function
CFLAGS += -DUNIT_TEST -DDEVELOPER
CFLAGS += -I../src -I../include
CFLAGS += $(shell pkg-config --cflags sdl3)

# Platform-specific SDL linking
ifeq ($(PLATFORM),windows)
    LDFLAGS = -lmingw32 -lSDL3 -lSDL3_mixer -lz -lpng -lzip -lmimalloc
else
    LDFLAGS = $(shell pkg-config --libs sdl3)
    LDFLAGS += -lSDL3_mixer -lz -lpng -lzip -lm -lmimalloc
endif

# SDL source files needed for testing
# Note: We use REAL I/O (sdl_image.c included) and only stub GPU operations
SDL_SRCS = ../src/sdl/sdl_test.c \
           ../src/sdl/sdl_core.c \
           ../src/sdl/sdl_texture.c \
           ../src/sdl/sdl_image.c \
           ../src/sdl/sdl_effects.c \
           ../src/sdl/sdl_draw.c

# Helper source files
HELPER_SRCS = ../src/game/memory.c \
              ../src/helper/helper.c \
              test_stubs.c

# Platform-specific memory implementation
ifeq ($(PLATFORM),linux)
    HELPER_SRCS += ../src/game/memory_linux.c
else ifeq ($(PLATFORM),macos)
    HELPER_SRCS += ../src/game/memory_macos.c
else
    HELPER_SRCS += ../src/game/memory_windows.c
endif

ALL_SRCS = $(SDL_SRCS) $(HELPER_SRCS)

# Output directory (test exes live in their own subfolder, not alongside the
# client binary in bin/). Resources are still read with the repo root as CWD.
BIN_DIR = ../bin/tests

# Test executables
TEST_SERIALIZED = $(BIN_DIR)/test_serialized
TEST_CONCURRENT = $(BIN_DIR)/test_concurrent
TEST_HASH_DIAG = $(BIN_DIR)/test_hash_distribution
TEST_RENDER_PRIMS = $(BIN_DIR)/test_render_primitives
TEST_SPRITE_CONFIG = $(BIN_DIR)/test_sprite_config
TEST_ZONEIO = $(BIN_DIR)/test_zoneio

all: $(TEST_SERIALIZED) $(TEST_CONCURRENT) $(TEST_HASH_DIAG) $(TEST_RENDER_PRIMS) $(TEST_SPRITE_CONFIG) $(TEST_ZONEIO)
test: run

$(TEST_SERIALIZED): test_texture_cache.c $(ALL_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CONCURRENT): test_texture_workers.c $(ALL_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_HASH_DIAG): test_hash_distribution.c $(ALL_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_RENDER_PRIMS): test_render_primitives.c $(ALL_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Sprite config test (no SDL dependencies, self-contained stubs)
SPRITE_CONFIG_SRCS = ../src/game/sprite_config.c ../src/lib/cjson/cJSON.c

$(TEST_SPRITE_CONFIG): test_sprite_config.c $(SPRITE_CONFIG_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter -DUNIT_TEST -I../src $^ -o $@

# Zone content parser test (no SDL dependencies, self-contained)
ZONEIO_SRCS = ../src/zoneio/zoneio.c

$(TEST_ZONEIO): test_zoneio.c $(ZONEIO_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter -DUNIT_TEST -I../src $^ -o $@

# Run serialized tests (single-threaded cache tests)
test_serialized: $(TEST_SERIALIZED)
	@echo ""
	@echo "==============================================="
	@echo "Running serialized tests (single-threaded)..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_serialized

# Run concurrent tests (multi-threaded worker tests)
test_concurrent: $(TEST_CONCURRENT)
	@echo ""
	@echo "==============================================="
	@echo "Running concurrent tests (multi-threaded)..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_concurrent

# Run hash distribution diagnostic (separate from main test suite)
test_hash_diag: $(TEST_HASH_DIAG)
	@echo ""
	@echo "==============================================="
	@echo "Running hash distribution diagnostics..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_hash_distribution

# Run render primitives tests
test_render_prims: $(TEST_RENDER_PRIMS)
	@echo ""
	@echo "==============================================="
	@echo "Running render primitives tests..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_render_primitives

# Run sprite config tests
test_sprite_config: $(TEST_SPRITE_CONFIG)
	@echo ""
	@echo "==============================================="
	@echo "Running sprite config tests..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_sprite_config

# Run zone content parser tests
test_zoneio: $(TEST_ZONEIO)
	@echo ""
	@echo "==============================================="
	@echo "Running zone content (zoneio) tests..."
	@echo "==============================================="
	cd .. && ./bin/tests/test_zoneio

# Run all tests in sequence
run: test_serialized test_concurrent test_hash_diag test_render_prims test_sprite_config test_zoneio
	@echo ""
	@echo "==============================================="
	@echo "All tests passed!"
	@echo "==============================================="

clean:
	rm -f $(TEST_SERIALIZED) $(TEST_CONCURRENT) $(TEST_HASH_DIAG) $(TEST_RENDER_PRIMS) $(TEST_SPRITE_CONFIG) $(TEST_ZONEIO) *.o

.PHONY: all clean run test_serialized test_concurrent test_render_prims test_sprite_config test_zoneio
