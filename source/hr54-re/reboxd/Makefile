CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation
RUNTIME = compat.c system.c module_auth.c package.c module_manager.c playback.c module_rpc.c module_process.c
LIB = json.c files.c module_manifest.c module_registry.c
.PHONY: all test clean
all: build/registry build/reboxd build/reboxd-host build/test-module build/module-fetch build/test-auth
build/registry: tests/registry.c $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ tests/registry.c $(LIB) -lm -pthread
build/reboxd: main.c http.c $(RUNTIME) $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ main.c http.c $(RUNTIME) $(LIB) -lm -pthread
test: all build/jellyfin-module build/iptv-module build/iptv-fetch
	python3 tests/foundation.py
	python3 tests/runtime.py
	python3 tests/installer.py
	python3 tests/jellyfin.py
	python3 tests/iptv.py
clean:
	rm -rf build

build/test-module: ../modules/test-module/main.c ../modules/shared/sdk.c http.c json.c files.c
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ $^ -lm -pthread

build/reboxd-host: main.c http.c $(RUNTIME) $(LIB) $(wildcard *.h)
	mkdir -p build
	$(CC) $(CFLAGS) -DREBOX_HOST_TEST -I. -o $@ main.c http.c $(RUNTIME) $(LIB) -lm -pthread

build/module-fetch: module_fetch.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< -lcurl

build/test-auth: tests/auth.c module_auth.c files.c json.c
	mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ $^ -lm -pthread

build/jellyfin-module: ../modules/jellyfin/main.c ../modules/jellyfin/http.inc ../modules/jellyfin/config.inc ../modules/jellyfin/library.inc ../modules/jellyfin/relay.inc ../modules/shared/sdk.c http.c json.c files.c module_auth.c
	mkdir -p build
	$(CC) $(CFLAGS) -DREBOX_HOST_TEST -Wno-unused-function -I. -o $@ ../modules/jellyfin/main.c ../modules/shared/sdk.c http.c json.c files.c module_auth.c -lm -pthread

build/iptv-module: ../modules/iptv/main.c ../modules/iptv/library.inc ../modules/iptv/relay.inc ../modules/iptv/clock.inc ../modules/shared/sdk.c http.c json.c files.c module_auth.c
	mkdir -p build
	$(CC) $(CFLAGS) -DREBOX_HOST_TEST -Wno-unused-function -I. -o $@ ../modules/iptv/main.c ../modules/shared/sdk.c http.c json.c files.c module_auth.c -lm -pthread

build/iptv-fetch: ../modules/iptv/fetch.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< -lcurl
