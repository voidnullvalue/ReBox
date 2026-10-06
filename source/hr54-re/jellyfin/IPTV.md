# Receiver-native IPTV — 2026-10-01

MEDIA → IPTV is a working browser and live player. Production remains on the
HR54; neither the development host nor `remote/server.py` is needed at runtime.
The supplied playlist is retained byte-for-byte. This release does **not** claim
that every listed channel is available or compatible.

## Playlist and library

Repository source: `jellyfin/iptv/eng.m3u` (476,555 bytes). Receiver source:
`/var/hr54-persist/jellyfin/iptv/eng.m3u`, mode 0600, directory mode 0700.
MD5: `7278fcda88bd83ad55bab06df3ac9fea`.

The native parent parses at startup and keeps a copy-on-write library inherited
by request workers. Before each request fork it checks mtime (including
nanoseconds), size and inode; a changed file is parsed into a replacement
library. A malformed replacement retains the previous valid library and
reports an error. No internet channel or logo is probed while loading metadata.

The actual file has **2,096 channels, 26 groups, 2,075 logos, 63 User-Agent
values, 30 Referer values, 59 URLs with queries**, and 127 duplicate-name pairs.
Every entry retains its `tvg-id`. IDs are `ch-` plus a 64-bit FNV-1a hash of
`tvg-id + newline + complete URL`; all 2,096 supplied IDs are distinct. Display
names are never identifiers. Names may contain commas. The EXTINF delimiter is
the first comma outside double quotes. `#EXTVLCOPT` applies to its pending entry
and overrides the corresponding EXTINF header attribute. Query strings and
header commas survive unchanged. The parser also retains `tvg-logo` and
`group-title`; logos are not fetched/displayed in this first release. No EPG was
added because the supplied header contains no XMLTV source.

Groups come from the file: Animation, Business, Classic, Comedy, Culture,
Documentary, Education, Entertainment, Food, General, Government, Kids,
Lifestyle, Local, Movies, News, Outdoors, Plex, Pluto TV, Rakuten TV, Samsung TV
Plus, Series, Shopping, Sports, Weather, Xumo.

## Navigation and APIs

IPTV root contains Search, All Channels and the parsed groups, sorted by name.
Root and channel lists show six rows at a time. UP/DOWN focuses a row; LEFT/RIGHT
changes page; SELECT opens a group or channel. Search reuses the existing
keyboard layout and handlers without changing Jellyfin keyboard state. Native
case-insensitive search matches name, tvg-id and group; only a result page is
returned to WebKit. BACK from results/channels goes to groups; BACK from groups
goes to MEDIA; MEDIA BACK exits ITV through the existing mechanism.

IPTV has its own localStorage key `media.iptv.state` and native checkpoint
`state/iptv-state.json`. It saves group, view (group/all/search), page, focus,
root page/focus, query, submitted query and channel ID. It never writes Jellyfin
`tv-state.json`, config or token, or Frigate's camera key. Playback return uses
`?source=iptv`, restores the previous list and focus, and can play another channel
without returning to MEDIA. The existing eight-second decoder-drain interval
is preserved. Loading can be cancelled with BACK/STOP.

- `GET /api/iptv/status`: library count, active flag, worker PID and last error.
- `GET /api/iptv/groups`: names/counts, no stream addresses or headers.
- `GET /api/iptv/channels?group=&query=&offset=&limit=`: paginated IDs/names,
  tvg IDs/groups and total. Omit group for all channels. Limit is bounded to 60.
- `GET/POST /api/iptv/state`: independently persisted browse checkpoint.
- `POST /api/iptv/play`, `{ "channelId": "ch-..." }`: resolves URL/headers only
  from the loaded local playlist. An arbitrary URL is not accepted.
- `POST /api/iptv/stop`: cancels preparation or stops active IPTV.
- Existing `/api/transport` STOP also stops IPTV. Pause/resume/seek are ignored
  for live sources. Status identifies `source: "iptv"`, `live: true`, channel ID,
  channel name and `returnToTv: true`.

## HTTPS and clock

`bin/hr54-iptv-fetch` is a roughly 10 KiB native adapter to the **installed**
`libcurl/7.52.1`, `wolfSSL/3.15.5` and uClibc. Its explicit MIPS O32 startup calls
`__uClibc_main`; it is linked against the extracted receiver ABI, not desktop
libc. The main backend remains the established static musl binary. No vendor
library, boot plugin, flash, Wi-Fi setting or recording storage was replaced.

The adapter supports DNS, SNI, redirects, peer-chain and hostname verification.
It uses the deployed `iptv/ca-certificates.crt` CA bundle. Verification stays on;
HTTPS redirects and HLS children cannot downgrade to HTTP. Only HTTP/HTTPS are
allowed, with at most five redirects. Exact playlist User-Agent/Referer values
are supplied to top-level, child-playlist and segment requests via argv, without
shell interpolation. The adapter's stdout carries effective URL, content type
and response bytes, so relative paths follow the final redirect target.

The receiver initially had an August 2024 Linux clock. Correcting it to current
UTC removed wolfSSL's certificate-date failure. To handle that stock cold-boot
condition without a host service, production startup now performs a bounded
SNTP request to `time.cloudflare.com:123` **only if the Linux clock is before
2026**. It checks connected peer, server mode/stratum, leap status and the echoed
random nonce, with two addresses and two-second waits. This is ordinary
unauthenticated SNTP, not authenticated time. Only the Linux software clock is
set; RTC/NVRAM/flash and boot scripts are untouched. If DNS/SNTP is unavailable,
startup logs that HTTPS requires a correct clock; verification is never disabled.
SNTP response validation is unit-tested. A controlled reboot on 2026-10-01
restored the native service, all 2,096 playlist entries, independent browse
checkpoints and Jellyfin authentication automatically. The post-boot clock
was already in 2026, so the old-clock SNTP correction branch was not exercised
by that reboot. See `tests/iptv-20261001/reboot-startup.log` and the before/after
state snapshots.

The existing TLS library has compatibility limits: France 24's tested source
failed its TLS handshake even with correct time. Al Jazeera's modern HTTPS CDN
worked. Expired and wrong-host certificates were rejected in receiver tests.
A CA refresh can replace the local PEM file without rebuilding either binary.

## Relay and compatibility

```
selected playlist channel ID
  → native server resolves trusted URL and headers
  → native curl/wolfSSL fetches HLS
  → master selection and live media-sequence tracking
  → bounded in-memory MPEG-TS segments
  → opaque local HTTP /iptv-stream/<random-session>.ts
  → existing hr54-play-url
  → stock Broadcom decoder → HDMI
```

The established fork-per-request server runs the long-lived TS response in a
separate request process. UI, STOP, Frigate and Jellyfin requests remain usable.
There is at most one IPTV session/claim, one relay worker and one active fetch
helper for that session. Session tokens are random; expired/reused claims fail.
STOP invalidates the token; reads poll cancellation every 100 ms, socket writes
have a three-second timeout, and helpers are killed/reaped. Helpers and native
request workers terminate if their parent dies. The MENU watcher also dies
with its listener, preventing restart leftovers. Inherited deployment-shell
file descriptors are closed at service startup.

Master selection excludes identifiable HEVC/VP9/AV1, HE-AAC and E-AC3; it prefers
the highest advertised compatible AVC variant at or below 8 Mbps. If none is
available, it selects the lowest compatible variant up to 16 Mbps. Unknown
CODECS metadata can be tried only within that bandwidth ceiling; the actual
segment PMT must identify H.264 (`stream_type 0x1b`) before output. Resolution
alone does not override bandwidth policy. A broken preferred variant currently
fails selection rather than automatically testing all alternatives.

Media playlists refresh at half target duration (target clamped 1–10 seconds).
Sequence numbers suppress already-sent segments, including overlap between
refreshes. Initial live playback starts within the latest three segments; VOD
ENDLIST is relayed once and terminates cleanly. Relative/root/protocol-relative
and query-only references are resolved without losing tokens. Segment PMTs are
checked for AVC. TS segments are concatenated without remuxing or transcoding.

Extensionless/PHP URLs are sniffed by initial bytes; HLS uses the parser and
continuous TS uses a streaming relay. HTTP and HTTPS TS both use the local relay,
so header behavior is consistent. Continuous TS must contain a recognizable
complete H.264 PMT in the initial data. Compatible AAC-LC/AC3 may be carried
unchanged. Separate audio playlists are not fetched/muxed; video-only is allowed.
No audible-audio claim was made for the representative channel.

DASH, SRT, fMP4/CMAF (`EXT-X-MAP` or non-TS bytes), encrypted HLS (anything other
than METHOD=NONE), and HLS byte ranges return useful unsupported errors. Split
PMTs without a complete recognized AVC section in a segment can also fail
safely. There is no DRM/key retrieval, SRT stack, DASH demuxer or CPU video
transcoder. Failed preparation leaves the browse page usable. Relay failure,
stall, missing player claim or EOF tears down the session, sends stock STOP and
returns to IPTV with the last error. No live video is saved on receiver disk.

Bounds: local playlist 2 MiB/10,000 entries; M3U line 16 KiB; URL 8 KiB;
User-Agent/Referer 2 KiB each; internet playlist 1 MiB/512 segments; segment
12 MiB; nested masters five; fetch connect timeout 12 seconds, ordinary fetch
20 seconds, low-speed timeout 15 seconds, stalled live session 40 seconds.
Buffers grow only within those caps and are freed after each segment.

## Proven evidence

[tests/iptv-20261001](tests/iptv-20261001/) contains deploy logs/hashes, parser
comparison, controlled HLS tests, frontend tests, TLS failures, raw receiver
logs, native relay logs, status/resource samples and source-return regressions.

On receiver `192.168.88.103`, **Al Jazeera English (1080p)** from the actual
supplied HTTPS URL was selected by remote-driven IPTV keyboard search `JAZ`.
The native master policy selected its 6,128,079 bps AVC variant. Native logs
identify MPEG-TS/H.264 PMT and successive fetched segment sequences. Stock logs
identify the loopback session URL, MPEG transport input, successful decoder
configuration, `notifyFramePresented#ENTER` and advancing positions. Status
samples progressed from 11 to 25 seconds while remaining active. STOP closed
networking and restored the same search page/focus. Reselecting played the same
channel; EXIT stopped it and returned to IPTV. BACK then walked to MEDIA/stock.
H.264 recognition evidence for this video is the relay's actual TS PMT check;
the updated trace additionally recognizes AAC and configures hardware audio on PID 259.

During that sample the relay worker RSS was 1,000–4,288 KiB; parent/watcher about
932/936 KiB. Linux free memory was 28,864–32,592 KiB, with approximately 19,544
KiB buffers and 175,180–175,196 KiB cached. This old kernel has no MemAvailable
field. Overall load was 2.26–2.45 (not isolated IPTV load). Worker plus reaped
TLS children advanced 238 CPU ticks over 14 seconds: approximately 17% of one
CPU at this musl/Linux 100-Hz accounting rate. The worker itself used 14 ticks;
most time was in TLS helpers. Only one IPTV relay worker was active. These are
short representative measurements, not a long-duration reliability claim.

Arirang TV (1080p) from the supplied plain-HTTP HLS URL also reached
MPEG-TS/H.264 recognition, decoder configuration, frame presentation and
continuing playback, followed by clean STOP. GTV's Referer-dependent source
returned an HTTP error during live testing; no playback success is claimed.
Controlled fixtures verify exact User-Agent and Referer forwarding. Actual
supplied DASH and SRT selections returned useful errors without active workers.

2GB's HTTPS master was fetched with verification and its 4.5 Mbps variant was
selected, but that child URL returned HTTP 404. France 24 failed native TLS.
Both entries remain in the library. Controlled fixtures prove HTTP HLS,
redirects, relative master selection, exact UA/Referer forwarding, overlapping
live refresh/deduplication, clean VOD EOF, concurrent control, STOP, unsupported
formats, mtime reload and ID enforcement. Every actual M3U entry is compared to
an independent Python parser, including all EXTVLCOPT associations. Synthetic
fixtures cover quoted commas and explicit-header precedence.

Jellyfin native host tests, Media Hub/keyboard/state tests and Doom frame tests
pass. Live regressions exercise real Frigate discovery/direct camera playback,
Jellyfin authentication/state, playback/pause/resume/seek/STOP and owner return.
MEDIA EXIT retains the existing rule: leave Jellyfin search/folders before
exiting MEDIA. No generic codec matrix was rerun.

## Build, deploy, replace and rollback

```sh
sh jellyfin/remote/build_iptv.sh
python3 jellyfin/remote/test/iptv_playlist_test.py
cc -O2 -o /tmp/hr54-iptv-fetch-host modules/iptv/fetch.c -lcurl
python3 jellyfin/remote/test/iptv_integration.py
bash jellyfin/remote/test/run_host_tests.sh
node jellyfin/remote/test/media_hub_test.js
node jellyfin/remote/test/doom_decode_test.js
python3 jellyfin/remote/deploy_iptv.py --receiver <verified-current-IP>
```

The build script uses `/tmp/hr54-zig/zig` and the existing extracted receiver
rootfs; `HR54_ZIG` and `HR54_SYSROOT` can override those paths. Host-only tests
need cc, Python, Node and libcurl. These are development dependencies, not
production dependencies. Receiver verification scripts contain this session's
verified address; update it deliberately before running them elsewhere.

Deployment extends the prior Media Hub procedure: live receiver/status identity,
idle checks before/after staging, installed production hashes, TFTP staging,
hash-verified `.next` copies, timestamped backups, sync/atomic rename and exact
listener/child PID restart. It avoids unavailable receiver `awk`, using `/proc`
PPid checks with grep. `remote/iptv-production-hashes.json` records the last
verified deployment; drift aborts before replacement. Config/token/Jellyfin state
are not deployment assets. Logs are `log/jf.log`, `log/tv-events.log` and stock
`/var/viewer/messages.log`.

To replace only the playlist: stop playback, stage the new file as
`iptv/eng.m3u.next` through the existing TFTP channel, check its hash, preserve
`eng.m3u` in a timestamped backup, chmod 0600, sync and rename atomically. The
next IPTV request reloads it; restarting the known native service also works.
No compiler is involved. Update the deployment manifest's playlist hash after
verifying this deliberate replacement, so a subsequent code deploy recognizes
the new baseline. Parser tests describe the original supplied file and therefore
intentionally assert its original count/metadata.

The latest pre-update backup is `backup/iptv-20261001-093052`, recorded in
`audio-fix-deployment.log` (the preceding release is in `final-deployment.log`).
The **first IPTV backup** `backup/iptv-20261001-082604` contains the working
pre-IPTV Media Hub binary and TV assets. Later backups contain working IPTV
versions. To roll back while idle, stage that backup's `hr54-jf`, `app.js`,
`app.css`, `index.html` to their original `.next` destinations, verify hashes,
sync/rename, stop only the current exact native listener and identified child
PIDs, then run the unchanged boot command from `remote/README.md`. The added
playlist/helper/CA files can remain unused; no auth/config rollback, reboot,
plugin replacement, flash write or recording deletion is needed.

## Audio startup correction (2026-10-01)

Al Jazeera English's selected 1080p TS contains non-silent stereo AAC-LC at
48 kHz (PID 259). Its first audio PES arrives about 1.3 MiB into the sampled
segment, behind the first large video access unit. Stock playURL detected
video but labeled audio `Invalid`. Simplifying PMT descriptors did not fix
it; moving the first complete audio PES before the first video PES did.
The receiver then reported AAC and configured `CdiAudioDecoder` type 6 on
the unchanged PID 259. The user heard audio during the diagnostic tests.

The native relay now performs that bounded packet reorder once, on the
first HLS TS segment. Every original packet, PID, timestamp, continuity
counter and encoded payload is preserved; per-PID order is unchanged.
There is no transcode, host playback dependency, or live receiver disk file.
The temporary host fixture was diagnostic only and has been shut down.
The extra temporary allocation is at most 256 TS packets (48,128 bytes).
Absent, incomplete or scrambled audio is left unchanged. Unit tests check
exact packet preservation/order, idempotence and these guards.

Evidence: `tests/iptv-20261001/audio-original.receiver.log`,
`audio-compact.receiver.log`, `audio-remux.receiver.log` and
`audio-fix-deployment.log`. The live HTTPS verification also requires
AAC recognition and hardware audio configuration. The final restored live session
is recorded in `audio-final-live.receiver.log`, `audio-final-live.native.log`
and `audio-final-live.status.json`.

Post-reboot playback also passed native HTTPS HLS, AAC recognition/hardware
audio configuration and video frame presentation. Evidence is
`tests/iptv-20261001/reboot-playback.receiver.log` and
`reboot-playback.status.json`. Al Jazeera was left playing for the user.
