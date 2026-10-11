#!/bin/sh
# Jeopardy module host acceptance.
#
# Stage 1: importer fixtures + authoritative C rules/state machine, question
# database and atomic persistence.
# Stage 2: the authoritative static-musl-ready LAN HTTP/WebSocket service, driven
# by raw Python websocket clients (joins, buzzer races, auth, replay/staleness,
# envelopes, timeouts, framing, privacy, reconnect/restart, full game).
# Stage 3: the native TV renderer stack (bounded JSON, redacted snapshot parse,
# scene render, remote mappings, QR encode+decode, lifecycle mock child), the
# real uClibc/EGL receiver build, default-seeding and the .rbox package.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
reboxd=$(CDPATH= cd -- "$root/../.." && pwd)/reboxd
shared=$(CDPATH= cd -- "$root/.." && pwd)/shared
ui=$(CDPATH= cd -- "$root/../../tools/native-ui" && pwd)
build="$root/build"
mkdir -p "$build"

echo "== importer =="
python3 "$here/test_import.py"

echo "== databases =="
python3 "$root/tools/import_questions.py" --pack "$root/tools/demo_questions.json" --out "$build/demo.jdb" --report
python3 "$root/tools/import_questions.py" \
  --jarchive "$here/fixtures/jarchive_game.json" \
  --tsv "$here/fixtures/jwolle_sample.tsv" \
  --out "$build/fixtures.jdb" --report
python3 "$root/tools/gen_large_db.py" --out "$build/large.jdb"
python3 "$root/tools/gen_gap_db.py" --out "$build/gap.jdb"

echo "== webpage =="
python3 "$root/tools/embed_assets.py" --source "$root/assets/index.html" --output "$build/web_assets.h"

echo "== font atlas =="
python3 "$root/tools/build_font_atlas.py" --font "$root/../../hr54-ui/assets/Overpass.ttf" --out "$root/src/jpq_font_data.h"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/jpq-test-XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "== host rules =="
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
  -Wwrite-strings -Wformat=2 -Wvla -Wno-unused-parameter \
  "$here/test_jeopardy.c" \
  "$root/src/jeopardy_db.c" "$root/src/jeopardy_rules.c" \
  "$root/src/jeopardy_persist.c" "$root/src/jeopardy_snapshot.c" \
  "$reboxd/json.c" "$reboxd/files.c" \
  -pthread -o "$build/test-jeopardy"

"$build/test-jeopardy" "$build/demo.jdb" "$build/fixtures.jdb" "$tmp" "$build/large.jdb" "$build/gap.jdb"

echo "== host service =="
${CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
  -Wwrite-strings -Wformat=2 -Wvla -Wno-unused-parameter -Wno-misleading-indentation \
  -I"$build" \
  "$root/src/main.c" "$root/src/jeopardy_service.c" "$root/src/jeopardy_net.c" \
  "$root/src/jeopardy_db.c" "$root/src/jeopardy_rules.c" \
  "$root/src/jeopardy_persist.c" "$root/src/jeopardy_snapshot.c" \
  "$root/src/jpq_lifecycle.c" \
  "$reboxd/json.c" "$reboxd/files.c" "$reboxd/http.c" "$shared/sdk.c" \
  -pthread -lm -o "$build/jeopardy-host"

echo "== native renderer host tests =="
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
  -I"$root/src" -I"$ui" \
  "$here/test_native.c" \
  "$root/src/jpq_view.c" "$root/src/jpq_parse.c" "$root/src/jpq_render.c" \
  "$root/src/jpq_font.c" "$root/src/jpq_qr.c" "$root/src/jpq_remote.c" \
  "$root/src/jpq_lifecycle.c" "$root/src/jpq_input.c" \
  "$ui/framebuffer.c" -o "$build/test-native"
${CC:-cc} -std=gnu11 -O1 -Wall -Wextra -I"$root/src" -I"$ui" \
  "$here/mock_engine.c" -o "$build/mock-engine"
JPQ_MOCK_ENGINE="$build/mock-engine" "$build/test-native"

echo "== renderer previews =="
mkdir -p "$build/preview"
"$build/test-native" --preview "$build/preview"
for scene in lobby board clue daily-double final final-judge results; do
  python3 "$ui/png_preview.py" "$build/preview/$scene.rgba" "$build/preview/$scene.png"
done
ls -l "$build/preview"/*.png

echo "== default seeding =="
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
  "$here/test_seed.c" "$shared/sdk.c" "$reboxd/json.c" "$reboxd/files.c" "$reboxd/http.c" \
  -pthread -lm -o "$build/test-seed"
"$build/test-seed"

echo "== native snapshot parse vs core =="
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
  -Wwrite-strings -Wformat=2 -Wvla -Wno-unused-parameter \
  -I"$root/src" \
  "$here/test_snapshot.c" \
  "$root/src/jeopardy_db.c" "$root/src/jeopardy_rules.c" \
  "$root/src/jeopardy_persist.c" "$root/src/jeopardy_snapshot.c" \
  "$root/src/jpq_view.c" "$root/src/jpq_parse.c" \
  "$reboxd/json.c" "$reboxd/files.c" \
  -pthread -o "$build/test-snapshot"
"$build/test-snapshot" "$build/demo.jdb"

echo "== network integration =="
python3 "$here/test_network.py" "$build/jeopardy-host" "$build/demo.jdb" "$build/large.jdb"

echo "== native remote through the live service =="
${CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
  -Wwrite-strings -Wformat=2 -Wvla -Wno-unused-parameter -Wno-misleading-indentation \
  -I"$build" -I"$root/src" \
  "$here/test_integration.c" "$root/src/jeopardy_service.c" "$root/src/jeopardy_net.c" \
  "$root/src/jeopardy_db.c" "$root/src/jeopardy_rules.c" \
  "$root/src/jeopardy_persist.c" "$root/src/jeopardy_snapshot.c" \
  "$root/src/jpq_view.c" "$root/src/jpq_parse.c" "$root/src/jpq_remote.c" \
  "$root/src/jpq_lifecycle.c" \
  "$reboxd/json.c" "$reboxd/files.c" "$reboxd/http.c" "$shared/sdk.c" \
  -pthread -lm -o "$build/test-integration"
"$build/test-integration" "$build/demo.jdb"

echo "== native lifecycle on the real SDK eventloop (mock child, no receiver) =="
python3 "$here/test_sdk_native.py" "$build/jeopardy-host" "$build/mock-engine" "$build/demo.jdb"

echo "== reboot persistence (generic launcher boot.d hook + LAN firewall hook) =="
python3 "$here/test_boot_persistence.py"

echo "== receiver build (musl service + uClibc renderer + .rbox) =="
HR54_ZIG="${HR54_ZIG:-/tmp/hr54-zig/zig}" \
HR54_SYSROOT="${HR54_SYSROOT:-$root/../../extracted/sdb4-rootfs}" \
  sh "$root/build-receiver.sh"

echo "== packaging =="
python3 "$here/package_test.py" "$build/jeopardy.rbox"

echo "== jeopardy module OK =="
