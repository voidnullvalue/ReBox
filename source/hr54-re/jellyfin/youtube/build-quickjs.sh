#!/bin/sh
# Run in unpacked QuickJS 2026-06-04 source. Host qjsc only builds REPL data.
set -eu
zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
make -j4 qjs CROSS_PREFIX=mips- \
 CC="$zig cc -target mips-linux-musleabi -mcpu=mips32" \
 HOST_CC=cc AR="$zig ar" LDFLAGS='-static -s'
# Host-created bytecode must be swapped for this big-endian target.
./host-qjsc -x -s -c -o repl.c -m repl.js
make qjs CROSS_PREFIX=mips- \
 CC="$zig cc -target mips-linux-musleabi -mcpu=mips32" \
 HOST_CC=cc AR="$zig ar" LDFLAGS='-static -s'
