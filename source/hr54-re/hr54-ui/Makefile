CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -I. -I../tools/native-ui
CORE = $(wildcard api/*.c) input/keys.c input/dispatcher.c apps/controller.c ui/animation.c ui/draw.c ui/image.c ui/text.c ui/icons.c ui/artwork.c ui/home.c ui/browser.c ui/keyboard.c ui/player_osd.c ui/settings.c ui/renderer.c ../tools/native-ui/framebuffer.c
.PHONY: all host receiver previews test clean
all: host
host: build/hr54-ui-host
build/hr54-ui-host: main.c apps/fixtures.c apps/preview_art.h $(CORE) platform.h ../tools/native-ui/presentation_policy.h $(wildcard api/*.h input/*.h apps/*.h ui/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ main.c apps/fixtures.c $(CORE) -lm -pthread
receiver:
	sh tools/build-receiver.sh
previews: host
	python3 tools/previews.py
test: host
	python3 tests/run.py
clean:
	rm -f build/hr54-ui-host build/hr54-ui build/test-ui

build/test-modules: tests/test_modules.c api/models.c api/modules.c api/json.c api/client.c api/modules.h api/models.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_modules.c api/models.c api/modules.c api/json.c api/client.c -lm

test-modules: build/test-modules
	./build/test-modules

.PHONY: test-modules test-module-integration
test-module-integration: host
	$(MAKE) -C ../reboxd all build/jellyfin-module
	python3 tests/modules_e2e.py
	python3 tests/jellyfin_e2e.py
