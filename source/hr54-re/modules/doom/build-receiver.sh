#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
doom_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
mkdir -p "$here/build/bin"
"$doom_zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
  -o "$here/build/bin/module" "$here/main.c" "$here/../shared/sdk.c" \
  "$project/reboxd/http.c" "$project/reboxd/json.c" "$project/reboxd/files.c" -pthread -lm
file "$here/build/bin/module"
