#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
radio_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
mkdir -p "$here/build/bin"
"$radio_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -o "$here/build/bin/module" \
 "$here/main.c" "$here/../shared/sdk.c" "$project/reboxd/http.c" "$project/reboxd/json.c" \
 "$project/reboxd/files.c" "$project/reboxd/module_auth.c" -pthread -lm

radio_ffmpeg=${REBOX_RADIO_FFMPEG:-$here/build/ffmpeg-8.0}
if [ ! -f "$radio_ffmpeg/configure" ]; then
 mkdir -p "$here/build"
 tar -xf "$project/../dependencies/ffmpeg-8.0.tar.xz" -C "$here/build"
fi
(cd "$radio_ffmpeg"; sh "$here/build-audio.sh")
