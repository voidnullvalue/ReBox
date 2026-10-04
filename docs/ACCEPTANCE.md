# Accept a new installation — physical and HDMI evidence required

Do not substitute registration ACKs, listening ports, `broker --check`, fake
frames or host unit tests for physical success. Observe your own receiver.

## Input

With native Home visible, press **RIGHT once**. Capture both:

```sh
HR54_DRAIN=5 tools/recv/hr54-shell.sh 'tail -n 45 /var/hr54-persist/native-menu/input-broker.log; tail -n 45 /var/hr54-persist/native-menu/startup.log'
```

Evidence must show actual vendor 47952 command-8 raw key/press/release,
broker interested consumer/queue/send, UI receive on loopback 47953, mapped
UiKey, controller action AND visible selection change. Current traced RF RIGHT:
12-byte frame, raw `0x0001e103` press, `0x0000e103` release, UiKey 4.
Then test UP/DOWN/LEFT/RIGHT, SELECT, BACK, MENU, GUIDE, EXIT and playback keys.
Some remotes share LEFT/BACK and PLAY/PAUSE; log the actual codes, not the labels
you expected. Context-sensitive LEFT/BACK and combined PLAY/PAUSE are repaired.

If nothing arrives at 47952 despite ACKs, inspect `rf4cerc`, the vendor
EnableDevice result and keydispatcher's `/dev/nds/rf4ce/zrcinput` descriptor.
Do not re-enable Druid just to restore radio initialization. Never reset pairing
unless there is independent evidence it is required.

## Android-equivalent Jellyfin playback

Log into your own Jellyfin account. Pick a known compatible item ID from YOUR
server; the original receiver's item IDs are not portable. Start idle at native
Home and record `/api/state` generation. Issue:

```sh
curl --max-time 60 -H 'Content-Type: application/json' \
  -d '{"itemId":"YOUR_ITEM_ID","startSeconds":0,"returnToTv":false}' \
  "http://$HR54_HOST:8130/api/play"
curl --max-time 10 "http://$HR54_HOST:8130/api/state"
HR54_DRAIN=3 tools/recv/hr54-shell.sh '/opt/middleware_core/system/tv/uconntest "<com.ucentric.pvruconnect.DirectTest command=\"getSpeed\" session=\"0\"/>"'
```

Require success, state `playing=true`, `phase=playing`, incremented generation,
DirectTest rate **1000**, native shell hidden, moving HDMI video with audio,
and NO stock checking-satellite/setup frame during playback. State/rate alone
cannot prove video is visible: the stale vendor I-frame previously hid valid
media. Idle/stopped getSpeed 0 is normal.

Physically MENU/GUIDE must reopen OUR full-width shell; EXIT must return to
video. Pause, wait, then resume and observe freeze/motion/audio. The repaired
combined e401 key toggles; a distinct e400 dedicated PLAY needs separate testing.
API paused state/HDMI freeze matter: the relay pause implementation can still
report vendor getSpeed 1000 while paused.

While playing, issue a second play request without stopping. Require new
generation and visible video/audio with no stock UI. Then:

```sh
curl --max-time 30 -H 'Content-Type: application/json' -d '{}' \
  "http://$HR54_HOST:8130/api/playback/stop"
curl --max-time 10 "http://$HR54_HOST:8130/api/state"
```

Require idle/phase idle, visible native Home and no stock setup. Repeat through
the ACTUAL Android app as well as curl. Curl semantic equivalence is not proof
of phone connectivity/configuration. Verify errors leave a native retry/error
state. Test each optional source independently before claiming it works.

## Persistence

After live acceptance, reboot normally. Record old/new
`/proc/sys/kernel/random/boot_id`, API readiness/idle, deployed hashes, Druid false,
native Home and a physical directional press through the complete chain. Test
playback again after reboot. A temporary vendor boot/satellite screen before
our late userland startup is possible; permanent failure to reach native Home
is NOT success. A warm reboot is not a cold power-cycle test.

Tracing is initially enabled. After acceptance it may be disabled by moving
`native-menu/INPUT_TRACE` into a backup and restarting the relevant OWNED
supervisors intentionally. Do not assume a UI kill always restarts it; confirm
new PIDs. Keep rollback snapshots and exact evidence of remaining limitations.
