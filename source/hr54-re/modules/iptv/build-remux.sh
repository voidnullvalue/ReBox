#!/bin/sh
# Run inside an unpacked FFmpeg 8.0 tree. Packet copy only.
set -eu
iptv_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
./configure --cc="$iptv_zig cc -target mips-linux-musleabi -mcpu=mips32" \
 --ar="$iptv_zig ar" --ranlib="$iptv_zig ranlib" --enable-cross-compile \
 --arch=mips --cpu=mips32 --target-os=linux --disable-everything \
 --disable-autodetect --disable-network --disable-doc --disable-debug \
 --disable-asm --disable-pthreads --disable-avdevice --disable-avfilter \
 --disable-swscale --disable-swresample --disable-programs \
 --enable-demuxer=mov --enable-muxer=mpegts --enable-protocol=file,pipe \
 --enable-bsf=h264_mp4toannexb --enable-parser=h264,aac --extra-ldflags=-static
make -j4 libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a
