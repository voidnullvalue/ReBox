#!/usr/bin/env bash
# Host-side end-to-end test of the receiver-native hr54-jf service against a
# mock Jellyfin.  Verifies routing, JSON, auth, browse/search, artwork,
# PlaybackInfo, the /play relay, transport gating, seek, stop, and TV state.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTDIR=${HR54_TESTDIR:-$(mktemp -d /tmp/rebox-jf-test.XXXXXX)}
if [ -e "$TESTDIR" ] && [ -n "$(ls -A "$TESTDIR")" ]; then
    echo 'Test directory must be new/empty; refusing to delete user data' >&2
    exit 1
fi
mkdir -p "$TESTDIR/persist" "$TESTDIR/docroot/tv" "$TESTDIR/out"
cp "$ROOT/static/tv/index.html" "$ROOT/static/tv/app.js" "$ROOT/static/tv/app.css" \
   "$TESTDIR/docroot/tv/"

export MOCK_APPROVE_FILE="$TESTDIR/approved"
export MOCK_REQUEST_LOG="$TESTDIR/mock-requests.log"
export MOCK_FETCH_OUT="$TESTDIR/fetch.out"
export MOCK_FETCH_PROGRESS="$TESTDIR/fetch.progress"
export JF_PERSIST_ROOT="$TESTDIR/persist"
export JF_PLAY_CMD="sh $ROOT/test/mock_playurl.sh"

python3 "$ROOT/test/mock_jellyfin.py" 18096 >"$TESTDIR/mock.log" 2>&1 &
MOCK_PID=$!
sleep 0.5
trap 'kill $MOCK_PID 2>/dev/null; pkill -f "hr54-jf-native $TESTDIR" 2>/dev/null' EXIT

BIN="$TESTDIR/hr54-jf-native"
cc -std=c11 -O1 -o "$BIN" "$ROOT/hr54_jf.c" || exit 1
cc -std=c11 -O1 -o "$TESTDIR/native-presentation-unit" "$ROOT/test/native_presentation_unit.c" || exit 1
mkdir -p "$TESTDIR/native-presentation-state"
"$TESTDIR/native-presentation-unit" "$TESTDIR/native-presentation-state" || exit 1

fails=0
check() { # check NAME EXPECTED_ACTUAL_STATUS
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1 (want $2 got $3)"; fails=$((fails+1)); fi
}

# Native quality policy unit checks use the same compiled request builder.
cc -std=c11 -O1 -o "$TESTDIR/quality-unit" "$ROOT/test/jellyfin_quality_unit.c" || exit 1
mkdir -p "$TESTDIR/quality-unit-state"
"$TESTDIR/quality-unit" "$TESTDIR/quality-unit-state" || exit 1

# --- Round 1: no token -------------------------------------------------
"$BIN" "$TESTDIR/docroot" 127.0.0.1 18096 18131 --no-launcher >"$TESTDIR/jf.log" 2>&1 &
JF_PID=$!
sleep 0.5
B=http://127.0.0.1:18131

check "status idle playing" "False" "$(curl -s $B/api/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["playing"])')"
check "auth unauthenticated" "False" "$(curl -s $B/api/auth/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["authenticated"])')"
check "libraries without token rejected" "sign in to Jellyfin first" "$(curl -s $B/api/libraries | python3 -c 'import json,sys;print(json.load(sys.stdin)["error"])')"
check "static index served" "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/)"
check "static app.js type" "application/javascript" "$(curl -s -o /dev/null -w '%{content_type}' $B/tv/app.js)"
check "unknown api 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' $B/api/nope)"
check "art without token 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' $B/art/lib1)"

# --- Quick Connect with approval ---------------------------------------
code=$(curl -s -X POST $B/api/auth/start | python3 -c 'import json,sys;print(json.load(sys.stdin)["code"])')
check "qc code" "MOCKQC" "$code"
pending=$(curl -s $B/api/auth/poll | python3 -c 'import json,sys;print(json.load(sys.stdin)["pending"])')
check "qc pending before approval" "True" "$pending"
touch "$MOCK_APPROVE_FILE"
sleep 0.3
authed=$(curl -s $B/api/auth/poll | python3 -c 'import json,sys;print(json.load(sys.stdin)["authenticated"])')
check "qc authenticated after approval" "True" "$authed"
check "qc user" "void" "$(curl -s $B/api/auth/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["user"])')"

# --- Browse / search ----------------------------------------------------
libs=$(curl -s $B/api/libraries | python3 -c 'import json,sys;d=json.load(sys.stdin);print(len(d["libraries"]), d["libraries"][1]["name"])')
check "libraries" "2 Shows & \"Things\"" "$libs"
items=$(curl -s "$B/api/items?videoOnly=1&limit=6&offset=0" | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["total"], d["items"][0]["playable"], d["items"][2]["playable"], len(d["items"][0]["overview"]))')
check "items page + virtual exclusion + overview truncation" "3 True False 400" "$items"
search=$(curl -s "$B/api/items?videoOnly=1&search=H%C3%A4ndel%20%26%20Co" | python3 -c 'import json,sys;print(json.load(sys.stdin)["total"])')
check "search with unicode and ampersand" "1" "$search"
grep -q "SearchTerm='Händel & Co'" "$TESTDIR/mock-requests.log" || true
grep "Users/Items" "$TESTDIR/mock-requests.log" | tail -1 | grep -q "SearchTerm='Händel & Co'" \
    && echo "ok   mock saw decoded search term" || { echo "FAIL mock search term"; fails=$((fails+1)); }

# --- TV state -----------------------------------------------------------
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"lib":"lib1","page":3,"searching":true,"submitted":"HANDBAN","query":"HANDBAN","zone":"card","index":2}' \
     $B/api/tv/state > /dev/null
state=$(curl -s $B/api/tv/state | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["lib"],d["page"],d["submitted"],d["zone"],d["index"])')
check "tv state round trip" "lib1 3 HANDBAN card 2" "$state"

# --- Artwork ------------------------------------------------------------
curl -s -o "$TESTDIR/art.jpg" -w '%{content_type}' $B/art/vid1.jpg >"$TESTDIR/art.ct"
artct=$(cat "$TESTDIR/art.ct")
artbytes=$(python3 -c 'd=open("'"$TESTDIR"'/art.jpg","rb").read(); print(d.startswith(bytes([0xff,0xd8])) and b"MOCKJPEGvid1" in d)')
check "art bytes + content type" "True image/jpeg" "$artbytes $artct"
curl -s -o /dev/null $B/art/vid1.jpg
artcalls=$(grep -c 'Art/vid1' "$TESTDIR/mock-requests.log" || echo 0)
check "art cached (one upstream fetch)" "1" "$artcalls"

# --- Play, gating, seek, stop -------------------------------------------
rm -f "$TESTDIR/fetch.out" "$TESTDIR/fetch.progress"
curl -s -X POST -d '{"itemId":"vid1","returnToTv":true}' $B/api/play > "$TESTDIR/play.json"
playres=$(python3 -c 'import json;d=json.load(open("'"$TESTDIR"'/play.json"));print(d["playing"], d["name"], d["itemId"])')
check "play response" "True Title One vid1" "$playres"
gen_play=$(python3 -c 'import json;d=json.load(open("'"$TESTDIR"'/play.json"));print(d["generation"])')
check "play response carries session identity" "yes" "$([ -n "$gen_play" ] && [ "$gen_play" -gt 0 ] && echo yes || echo no)"
sleep 1.5
check "live lifecycle phase in state" "playing" "$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["phase"])')"
check "state shows committed generation" "$gen_play" "$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["generation"])')"
grep -q "TranscodeGet/vid1" "$TESTDIR/mock-requests.log" && echo "ok   relay requested upstream stream" \
    || { echo "FAIL relay upstream request"; fails=$((fails+1)); }
paused=$(curl -s -X POST -d '{}' $B/api/playback/pause | python3 -c 'import json,sys;print(json.load(sys.stdin)["paused"])')
check "pause gates" "True" "$paused"
check "repeat pause reports actual state" "True" "$(curl -s -X POST -d '{}' $B/api/playback/pause | python3 -c 'import json,sys;print(json.load(sys.stdin)["paused"])')"
check "aggregate Jellyfin state" "jellyfin Title One True True" "$(curl -s $B/api/state | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["source"],d["title"],d["paused"],d["transport"]["seek"])')"
p1=$(awk '{print $1}' "$TESTDIR/fetch.progress" 2>/dev/null || echo 0)
sleep 1.2
p2=$(awk '{print $1}' "$TESTDIR/fetch.progress" 2>/dev/null || echo 0)
check "bytes stop flowing while paused" "yes" "$([ "$p1" = "$p2" ] && echo yes || echo no)"
status=$(curl -s $B/api/status | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["playing"],d["paused"],d["elapsed"]>0)')
check "status shows paused playing" "True True True" "$status"
# A quality save during playback does not restart it; a later restart uses it.
curl -s -X POST -d '{"jellyfinVideoBitrate":16000000}' $B/api/settings >/dev/null
check "quality save keeps active playback" "True True" "$(curl -s $B/api/status | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["playing"],d["paused"])')"
check "quality save does not renegotiate active stream" "1" "$(grep -c 'PlaybackInfo/vid1' "$TESTDIR/mock-requests.log")"
resumed=$(curl -s -X POST -d '{}' $B/api/playback/resume | python3 -c 'import json,sys;print(json.load(sys.stdin)["paused"])')
check "resume ungates" "False" "$resumed"
sleep 1.0
p3=$(awk '{print $1}' "$TESTDIR/fetch.progress" 2>/dev/null || echo 0)
check "bytes flow after resume" "yes" "$([ "$p3" -gt "$p2" ] && echo yes || echo no)"
curl -s -X POST -d '{}' $B/api/playback/pause >/dev/null
sleep 21
res=$(curl -s -X POST -d '{}' $B/api/playback/resume | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d.get("resumed"), d["playing"])')
check "long-pause resume restarts stream" "True True" "$res"
gen_resume=$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["generation"])')
check "restart after long pause commits replacement session" "yes" "$([ "$gen_resume" -gt "$gen_play" ] && echo yes || echo no)"
check "next restart uses changed ceiling" "yes" "$(grep 'Quality/vid1' "$TESTDIR/mock-requests.log" | tail -1 | grep -q 'top=16000000 profile=16000000' && echo yes || echo no)"
grep -c "PlaybackInfo/vid1" "$TESTDIR/mock-requests.log" | grep -q "^2$" && echo "ok   second PlaybackInfo for restart" \
    || { echo "FAIL second PlaybackInfo"; fails=$((fails+1)); }
seek=$(curl -s -X POST -d '{"seconds":600,"delta":-30}' $B/api/playback/seek | python3 -c 'import json,sys;print(json.load(sys.stdin)["startSeconds"])')
seek=$(curl -s -X POST -d '{"seconds":600,"delta":-30}' $B/api/playback/seek | tee "$TESTDIR/seek.json" | python3 -c 'import json,sys;print(json.load(sys.stdin)["startSeconds"])')
check "seek restarts at offset" "570" "$seek"
gen_seek_state=$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["generation"])')
check "seek commits replacement session" "yes" "$([ "$gen_seek_state" -gt "$gen_resume" ] && echo yes || echo no)"
check "seek response identity matches state" "$gen_seek_state" "$(python3 -c 'import json;d=json.load(open("'"$TESTDIR"'/seek.json"));print(d["generation"])')"
grep "TranscodeGet" "$TESTDIR/mock-requests.log" | tail -1 | grep -q "StartTimeTicks=" \
    && echo "ok   seek appended StartTimeTicks" || { echo "FAIL StartTimeTicks"; fails=$((fails+1)); }
stopped=$(curl -s -X POST -d '{}' $B/api/playback/stop | python3 -c 'import json,sys;print(json.load(sys.stdin)["stopped"])')
check "stop" "True" "$stopped"
sleep 0.5
fsize=$(stat -c%s "$TESTDIR/fetch.out" 2>/dev/null || echo 0)
full=$((6 * 1024 * 1024))
check "stop closed relay early (<full stream)" "yes" "$([ "$fsize" -lt "$full" ] && echo yes || echo no)"
check "status idle after stop" "False" "$(curl -s $B/api/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["playing"])')"
check "idle lifecycle phase after stop" "idle" "$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["phase"])')"
check "stop does not commit a session" "$gen_seek_state" "$(curl -s $B/api/state | python3 -c 'import json,sys;print(json.load(sys.stdin)["generation"])')"

curl -s -X POST -d '{"jellyfinVideoBitrate":12000000}' $B/api/settings >/dev/null
# --- Quality: real requests, next-play semantics and persistence --------
check "default quality" "12000000" "$(curl -s $B/api/settings | python3 -c 'import json,sys;print(json.load(sys.stdin)["settings"]["jellyfinVideoBitrate"])')"
cp "$TESTDIR/persist/config/token" "$TESTDIR/token-before-quality"
cp "$TESTDIR/persist/config/config.json" "$TESTDIR/config-before-quality"
cp "$TESTDIR/persist/state/tv-state.json" "$TESTDIR/tv-before-quality"
for rate in 8000000 12000000 16000000; do
    curl -s -X POST -d "{\"jellyfinVideoBitrate\":$rate}" $B/api/settings > /dev/null
    curl -s -X POST -d '{"itemId":"vid1"}' $B/api/play > /dev/null
    sleep 0.5
    check "both PlaybackInfo ceilings $rate" "yes" "$(grep 'Quality/vid1' "$TESTDIR/mock-requests.log" | tail -1 | grep -q "top=$rate profile=$rate videoCopy=True audioCopy=False" && echo yes || echo no)"
    curl -s -X POST -d '{}' $B/api/playback/stop >/dev/null
    sleep 0.3
done
cmp -s "$TESTDIR/token-before-quality" "$TESTDIR/persist/config/token" && echo "ok   quality preserves token" || fails=$((fails+1))
cmp -s "$TESTDIR/config-before-quality" "$TESTDIR/persist/config/config.json" && echo "ok   quality preserves login config" || fails=$((fails+1))
cmp -s "$TESTDIR/tv-before-quality" "$TESTDIR/persist/state/tv-state.json" && echo "ok   quality preserves browse state" || fails=$((fails+1))
kill $JF_PID 2>/dev/null
sleep 0.3
"$BIN" "$TESTDIR/docroot" 127.0.0.1 18096 18131 --no-launcher >>"$TESTDIR/jf.log" 2>&1 &
JF_PID=$!
sleep 0.5
check "quality survives service restart" "16000000" "$(curl -s $B/api/settings | python3 -c 'import json,sys;print(json.load(sys.stdin)["settings"]["jellyfinVideoBitrate"])')"
check "auth survives quality/service restart" "True" "$(curl -s $B/api/auth/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["authenticated"])')"

# --- Logout -------------------------------------------------------------
logout=$(curl -s -X POST $B/api/auth/logout | python3 -c 'import json,sys;print(json.load(sys.stdin)["authenticated"])')
check "logout" "False" "$logout"
[ ! -f "$TESTDIR/persist/config/token" ] && echo "ok   token file removed" || { echo "FAIL token file kept"; fails=$((fails+1)); }

kill $JF_PID 2>/dev/null
echo
if [ "$fails" = 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "$fails FAILURES"; exit 1; fi
