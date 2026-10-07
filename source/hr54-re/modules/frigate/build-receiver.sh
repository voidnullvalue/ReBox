#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
frigate_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
mkdir -p "$here/build/bin"
"$frigate_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
  -o "$here/build/bin/module" "$here/main.c" "$here/../shared/sdk.c" \
  "$project/reboxd/http.c" "$project/reboxd/json.c" "$project/reboxd/files.c" \
  "$project/reboxd/module_auth.c" -pthread -lm
file "$here/build/bin/module"
# Reuse the same minimal FFmpeg build as YouTube (MOV demuxer, TS muxer,
# file/pipe protocols, no network, decoders or encoders). See build-remux.sh.
frigate_ffmpeg=${HR54_FFMPEG_BUILD:?Set HR54_FFMPEG_BUILD to the configured minimal FFmpeg 8.0 build directory}
"$frigate_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -s \
  -I"$frigate_ffmpeg" -o "$here/build/bin/relay" "$here/relay.c" \
  "$frigate_ffmpeg/libavformat/libavformat.a" \
  "$frigate_ffmpeg/libavcodec/libavcodec.a" \
  "$frigate_ffmpeg/libavutil/libavutil.a" -lm
file "$here/build/bin/relay"
