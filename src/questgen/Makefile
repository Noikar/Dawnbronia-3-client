# questgen - quest builder backend (Phase 3a: LiveQuest script emitter).
# Pure text tool: no SDL, no client core - just JSON in, command script out.
# Reuses the in-repo cJSON. Build:  make -C /e/Astonia/astonia_community_client/src/questgen

CC = clang
CFLAGS = -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter -I../lib/cjson

BIN = ../../bin/questgen.exe
SRCS = questgen.c ../lib/cjson/cJSON.c

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN)

clean:
	rm -f $(BIN)

.PHONY: clean
