#!/bin/sh
# Receiver uClibc link, following the working IPTV fetch startup recipe.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
hr54_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
hr54_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
case ${1:-smoke} in
    smoke) hr54_sources="$here/hr54-native-smoke.c $here/native_egl.c $here/framebuffer.c $here/receiver_bindings.c"; hr54_name=hr54-native-smoke ;;
    inspect) hr54_sources="$here/hr54-drawlist-inspect.c"; hr54_name=hr54-drawlist-inspect ;;
    *) echo 'usage: build-receiver.sh [smoke|inspect]' >&2; exit 2 ;;
esac
hr54_out=${OUT:-$here/build/$hr54_name}
mkdir -p "$(dirname -- "$hr54_out")"
set -x
"$hr54_zig" cc -target mips-linux-musleabi -mcpu=mips32 -msoft-float \
    -O2 -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
    -nostdinc -isystem "$(dirname -- "$hr54_zig")/lib/include" -DHR54_RECEIVER \
    -nostdlib -Wl,-s -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
    -Wl,--allow-shlib-undefined -Wl,-rpath,/opt/opengl/lib:/opt/sys_monitor/lib \
    -o "$hr54_out" $hr54_sources "$here/receiver_start.c" \
    "$hr54_sysroot/opt/opengl/lib/libopengl.so.1" "$hr54_sysroot/lib/libc.so.0" \
    "$hr54_sysroot/lib/librt.so.0"
set +x
file "$hr54_out"
readelf -h -l -d "$hr54_out"
