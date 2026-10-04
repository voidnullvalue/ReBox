#!/bin/sh
# Build existing native backend and tiny adapter to installed receiver TLS libs.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
iptv_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
iptv_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
"$iptv_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
    -o "$here/bin/hr54-jf" "$here/hr54_jf.c"
"$iptv_zig" cc -target mips-linux-musleabi -mcpu=mips32 -O2 \
    -fno-stack-protector -fno-builtin-fprintf -DHR54_UCLIBC_START -nostdlib \
    -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
    -Wl,--allow-shlib-undefined -o "$here/bin/hr54-iptv-fetch" \
    "$here/hr54_iptv_fetch.c" \
    "$iptv_sysroot/usr/lib/libcurl.so.4.4.0" "$iptv_sysroot/lib/libc.so.0"
