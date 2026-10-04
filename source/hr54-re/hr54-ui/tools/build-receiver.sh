#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project=$(CDPATH= cd -- "$here/.." && pwd)
hr54_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
hr54_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
ui="$project/tools/native-ui"
cd "$here"
mkdir -p build
"$hr54_zig" cc -target mips-linux-musleabi -mcpu=mips32 -msoft-float \
 -O2 -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -ffreestanding -fno-stack-protector -fno-builtin-fprintf \
 -nostdinc -isystem "$(dirname -- "$hr54_zig")/lib/include" -I"$ui/receiver-include" -I"$ui" -I. -DHR54_RECEIVER \
 -nostdlib -Wl,-s -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
 -Wl,--allow-shlib-undefined -Wl,-rpath,/opt/opengl/lib:/opt/sys_monitor/lib \
 -o build/hr54-ui main.c api/*.c input/keys.c input/dispatcher.c apps/controller.c ui/*.c \
 "$ui/native_egl.c" "$ui/framebuffer.c" "$ui/receiver_bindings.c" "$ui/receiver_start.c" \
 "$hr54_sysroot/opt/opengl/lib/libopengl.so.1" "$hr54_sysroot/lib/libpthread.so.0" "$hr54_sysroot/lib/libc.so.0" "$hr54_sysroot/lib/librt.so.0" "$hr54_sysroot/lib/libm.so.0"
file build/hr54-ui
readelf -h -l -d build/hr54-ui
"$hr54_zig" cc -target mips-linux-musleabi -mcpu=mips32 -msoft-float \
 -O2 -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -ffreestanding -fno-stack-protector \
 -nostdinc -isystem "$(dirname -- "$hr54_zig")/lib/include" -I"$ui/receiver-include" -I"$ui" -I. -DHR54_RECEIVER \
 -nostdlib -Wl,-s -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
 -o build/hr54-input-broker input/broker.c input/dispatcher.c input/keys.c \
 "$ui/receiver_start.c" "$hr54_sysroot/lib/libc.so.0" "$hr54_sysroot/lib/librt.so.0"
file build/hr54-input-broker
