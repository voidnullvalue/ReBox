CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation
RUNTIME = playback.c module_rpc.c module_process.c
LIB = json.c files.c module_manifest.c module_registry.c
.PHONY: all test clean
all: build/registry build/reboxd build/reboxd-host build/test-module
build/registry: tests/registry.c $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ tests/registry.c $(LIB) -lm -pthread
build/reboxd: main.c http.c $(RUNTIME) $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ main.c http.c $(RUNTIME) $(LIB) -lm -pthread
test: all
	python3 tests/foundation.py
	python3 tests/runtime.py
clean:
	rm -rf build

build/test-module: ../modules/test-module/main.c ../modules/shared/sdk.c http.c json.c files.c
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ $^ -lm -pthread

build/reboxd-host: main.c http.c $(RUNTIME) $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -DREBOX_HOST_TEST -I. -o $@ main.c http.c $(RUNTIME) $(LIB) -lm -pthread
