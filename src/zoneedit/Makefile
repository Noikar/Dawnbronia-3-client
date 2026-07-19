# Standalone zoneedit build (Windows / MSYS2 CLANG64).
#
# zoneedit reuses the client's sprite/render code by compiling the same source
# files (src/sdl/*.c + src/game/render.c) plus the zoneio parser. The client
# core is NOT modified. Invoke from this directory:
#
#   make -C src/zoneedit          # builds ../../bin/zoneedit.exe
#   make -C src/zoneedit run      # builds and runs (from the client root)
#
# It must run with the client root as the working directory so the reused
# loader finds res/gxN.zip (the `run` target handles that).

CC = clang
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CFLAGS += -I../../src -I../../include
CFLAGS += $(shell pkg-config --cflags sdl3)

LDFLAGS = $(shell pkg-config --libs sdl3) -lSDL3_mixer -lz -lpng -lzip -lmimalloc

BIN = ../../bin/zoneedit.exe

# zoneedit-specific sources
EDIT_SRCS = zoneedit.c zoneedit_stubs.c ../zoneio/zoneio.c

# Reused client sources (compiled, not modified)
REUSE_SRCS = ../sdl/sdl_core.c ../sdl/sdl_texture.c ../sdl/sdl_image.c ../sdl/sdl_effects.c \
             ../sdl/sdl_draw.c ../game/render.c ../game/font.c ../game/memory.c \
             ../game/memory_windows.c ../helper/helper.c

SRCS = $(EDIT_SRCS) $(REUSE_SRCS)

all: $(BIN)

$(BIN): $(SRCS)
	@mkdir -p ../../bin
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(BIN)

# Run from the client root so res/gxN.zip resolves.
run: $(BIN)
	cd ../.. && ./bin/zoneedit.exe

clean:
	rm -f $(BIN)

.PHONY: all run clean
