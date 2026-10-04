#!/bin/sh
# Run in an unpacked FFmpeg 8.0 source tree; outputs a static no-encoder helper.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
./configure --cc="$zig cc -target mips-linux-musleabi -mcpu=mips32" \
 --ar="$zig ar" --ranlib="$zig ranlib" --enable-cross-compile \
 --arch=mips --cpu=mips32 --target-os=linux --disable-everything \
 --disable-autodetect --disable-network --disable-doc --disable-debug \
 --disable-asm --disable-pthreads --disable-avdevice --disable-avfilter \
 --disable-swscale --disable-swresample --disable-programs \
 --enable-demuxer=mov --enable-muxer=mpegts --enable-protocol=file,pipe \
 --enable-bsf=h264_mp4toannexb --enable-parser=h264,aac --extra-ldflags=-static
make -j4 libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a
"$zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -s -I. \
 "$src/remux.c" libavformat/libavformat.a libavcodec/libavcodec.a \
 libavutil/libavutil.a -lm -o hr54-remux
