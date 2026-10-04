#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${HR54_YOUTUBE_BUILD:-/tmp/hr54-youtube-build}
zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
mkdir -p "$out"
cp "$here/yt-dlp.sh" "$out/yt-dlp-wrapper"
cp "$here/yt-dlp.conf" "$out/yt-dlp.conf"
for helper in relay exec-guard measure; do
 "$zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 -s -o "$out/$helper" "$here/$helper.c"
done
python3.14 - "$here" "$out" <<'PY'
import pathlib,sys,py_compile
for name in ('resolver','updater'):
 py_compile.compile(str(pathlib.Path(sys.argv[1])/(name+'.py')),cfile=str(pathlib.Path(sys.argv[2])/(name+'.pyc')),dfile=name+'.py',invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
PY
