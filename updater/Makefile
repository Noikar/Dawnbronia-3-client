# Standalone updater/launcher (Windows). Independent of the main client build:
# no SDL, no Rust, no vendored DLLs — just WinHTTP + BCrypt + cJSON.
#
# Build in an MSYS2 CLANG64 shell (produces a native console updater.exe):
#   MSYSTEM=CLANG64 /c/msys64/usr/bin/bash.exe -lc \
#     "cd /e/Astonia/astonia_community_client/updater && make"

CC       ?= clang
CFLAGS   ?= -O2 -Wall -Wextra -std=c11
INCLUDES  = -I../src
LDLIBS    = -lwinhttp -lbcrypt

# http.h + http_windows.c live here (updater-only); cJSON is shared with the client.
SRC = updater_main.c http_windows.c ../src/lib/cjson/cJSON.c

updater.exe: $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRC) $(LDLIBS) -o $@

clean:
	rm -f updater.exe

.PHONY: clean
