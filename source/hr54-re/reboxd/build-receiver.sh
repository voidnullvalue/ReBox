#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/.." && pwd)
rebox_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
rebox_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
cd "$here"
mkdir -p build/receiver
"$rebox_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -DREBOX_RECEIVER \
  -o build/receiver/reboxd main.c http.c json.c files.c module_manifest.c module_registry.c \
  module_rpc.c module_process.c module_auth.c package.c module_manager.c playback.c -pthread -lm
"$rebox_zig" cc -target mips-linux-musleabi -mcpu=mips32 -O2 -fno-stack-protector \
  -DREBOX_RECEIVER -DHR54_UCLIBC_START -nostdlib -Wl,-e,__start \
  -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -Wl,--allow-shlib-undefined \
  -o build/receiver/module-fetch module_fetch.c \
  "$rebox_sysroot/usr/lib/libcurl.so.4.4.0" "$rebox_sysroot/lib/libc.so.0"
file build/receiver/reboxd build/receiver/module-fetch
