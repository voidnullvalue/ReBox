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
iptv_ffmpeg=${HR54_FFMPEG_BUILD:-$here/build/ffmpeg-8.0}
if [ ! -f "$iptv_ffmpeg/configure" ]; then
  tar -xf "$project/../dependencies/ffmpeg-8.0.tar.xz" -C "$here/build"
fi
if [ ! -f "$iptv_ffmpeg/libavformat/libavformat.a" ]; then
  (cd "$iptv_ffmpeg"; sh "$here/build-remux.sh")
fi
"$iptv_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -s \
  -I"$iptv_ffmpeg" -o "$here/build/bin/remux" "$here/remux.c" \
  "$iptv_ffmpeg/libavformat/libavformat.a" \
  "$iptv_ffmpeg/libavcodec/libavcodec.a" \
  "$iptv_ffmpeg/libavutil/libavutil.a" -lm
file "$here/build/bin/remux"
