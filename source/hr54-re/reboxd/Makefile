CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation
LIB = json.c files.c module_manifest.c module_registry.c
.PHONY: all test clean
all: build/registry build/reboxd
build/registry: tests/registry.c $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ tests/registry.c $(LIB) -lm -pthread
build/reboxd: main.c http.c $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ main.c http.c $(LIB) -lm -pthread
test: all
	python3 tests/foundation.py
clean:
	rm -rf build
