CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CORE = framebuffer.c menu_model.c surface.c
HEADERS = framebuffer.h menu_model.h surface.h
BUILD = build
.PHONY: all test preview receiver clean
all: $(BUILD)/demo $(BUILD)/test_native_ui $(BUILD)/receiver_bindings.o
$(BUILD):
	mkdir -p $@
$(BUILD)/demo: demo.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) demo.c $(CORE) -o $@
$(BUILD)/test_native_ui: test_native_ui.c $(CORE) $(HEADERS) native_egl.c native_egl.h compositor_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) test_native_ui.c $(CORE) native_egl.c -o $@
$(BUILD)/receiver_bindings.o: receiver_bindings.c native_egl.h surface.h framebuffer.h | $(BUILD)
	$(CC) $(CFLAGS) -c receiver_bindings.c -o $@
test: all
	./$(BUILD)/test_native_ui
preview: $(BUILD)/demo
	./$(BUILD)/demo $(BUILD)/menu.rgba
	python3 png_preview.py $(BUILD)/menu.rgba samples/menu.png
	python3 png_preview.py $(BUILD)/menu.rgba samples/menu-over-video.png --video-context
clean:
	rm -rf $(BUILD)
receiver:
	./build-receiver.sh
