#!/bin/sh
# Run inside an unpacked FFmpeg 8.0 tree. No networking in this helper.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
radio_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
./configure --cc="$radio_zig cc -target mips-linux-musleabi -mcpu=mips32" \
 --ar="$radio_zig ar" --ranlib="$radio_zig ranlib" --enable-cross-compile --arch=mips --cpu=mips32 --target-os=linux \
 --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug --disable-asm --disable-pthreads \
 --disable-avdevice --disable-avfilter --disable-swscale --disable-programs --enable-swresample \
 --enable-demuxer=mp3,aac,mpegts,mov,ogg,flac --enable-muxer=mpegts --enable-protocol=pipe,file \
 --enable-parser=mpegaudio,aac,h264,opus,vorbis,flac --enable-decoder=aac_fixed,mp3,h264,opus,vorbis,flac --enable-encoder=ac3_fixed --extra-ldflags=-static
# FFmpeg 8.0's disabled-everything H.264 selection omits this dependency.
make -j4 libavformat/libavformat.a libavcodec/libavcodec.a libavcodec/bswapdsp.o libswresample/libswresample.a libavutil/libavutil.a
mkdir -p "$here/build/bin"
"$radio_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O3 -s -I. "$here/audio-remux.c" \
 libavformat/libavformat.a libavcodec/libavcodec.a libavcodec/bswapdsp.o libswresample/libswresample.a libavutil/libavutil.a -lm -o "$here/build/bin/audio-remux"
