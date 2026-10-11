#!/bin/sh
# Build the Jeopardy module: static-musl authoritative service + uClibc/EGL
# native renderer, then package the installable .rbox. Same verified receiver
# target as the games module (mips-linux-musleabi, mips32, msoft-float, vendor
# minimal headers); no desktop frameworks or dependencies.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../.." && pwd)
ui="$project/tools/native-ui"
zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
mkdir -p "$here/build/bin"

# The demo question database bundled as the module default.
if [ ! -f "$here/build/demo.jdb" ]; then
    python3 "$here/tools/import_questions.py" --pack "$here/tools/demo_questions.json" \
        --out "$here/build/demo.jdb" >/dev/null
fi
# Embed the self-contained phone webpage into the service binary.
python3 "$here/tools/embed_assets.py" --source "$here/assets/index.html" \
    --output "$here/build/web_assets.h" >/dev/null

echo "== service (static musl) =="
"$zig" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
  -I"$here/build" \
  -o "$here/build/bin/module" "$here/src/main.c" \
  "$here/src/jeopardy_service.c" "$here/src/jeopardy_net.c" \
  "$here/src/jeopardy_db.c" "$here/src/jeopardy_rules.c" \
  "$here/src/jeopardy_persist.c" "$here/src/jeopardy_snapshot.c" \
  "$here/src/jpq_lifecycle.c" \
  "$here/../shared/sdk.c" \
  "$project/reboxd/http.c" "$project/reboxd/json.c" "$project/reboxd/files.c" \
  -pthread -lm

echo "== native renderer (uClibc / EGL) =="
"$zig" cc -target mips-linux-musleabi -mcpu=mips32 -msoft-float \
  -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-misleading-indentation \
  -ffreestanding -fno-stack-protector -fno-builtin-fprintf \
  -nostdinc -isystem "$(dirname "$zig")/lib/include" -I"$ui/receiver-include" -I"$ui" -DHR54_RECEIVER \
  -nostdlib -Wl,-s -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
  -Wl,--allow-shlib-undefined -Wl,-rpath,/opt/opengl/lib:/opt/sys_monitor/lib \
  -o "$here/build/bin/hr54-jeopardy-native" \
  "$here/src/native_main.c" "$here/src/jpq_view.c" "$here/src/jpq_parse.c" \
  "$here/src/jpq_render.c" "$here/src/jpq_font.c" "$here/src/jpq_qr.c" \
  "$here/src/jpq_remote.c" "$here/src/jpq_input.c" \
  "$ui/native_egl.c" "$ui/framebuffer.c" "$ui/receiver_bindings.c" "$ui/receiver_start.c" \
  "$sysroot/opt/opengl/lib/libopengl.so.1" "$sysroot/lib/libc.so.0" "$sysroot/lib/librt.so.0" "$sysroot/lib/libm.so.0"

python3 "$here/package.py"
file "$here/build/bin/module" "$here/build/bin/hr54-jeopardy-native"
wc -c "$here/build/bin/module" "$here/build/bin/hr54-jeopardy-native" "$here/build/jeopardy.rbox"
