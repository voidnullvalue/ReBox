#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
iptv_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
iptv_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
mkdir -p "$here/build/bin"
"$iptv_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
  -o "$here/build/bin/module" "$here/main.c" "$here/../shared/sdk.c" \
  "$project/reboxd/http.c" "$project/reboxd/json.c" "$project/reboxd/files.c" \
  "$project/reboxd/module_auth.c" -pthread -lm
file "$here/build/bin/module"
"$iptv_zig" cc -target mips-linux-musleabi -mcpu=mips32 -O2 \
  -fno-stack-protector -fno-builtin-fprintf -DHR54_UCLIBC_START -nostdlib \
  -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
  -Wl,--allow-shlib-undefined -o "$here/build/bin/fetch" "$here/fetch.c" \
  "$iptv_sysroot/usr/lib/libcurl.so.4.4.0" "$iptv_sysroot/lib/libc.so.0"
file "$here/build/bin/fetch"
