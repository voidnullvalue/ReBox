# HR54 standalone YouTube — 2026-10-01

YouTube is the fourth MEDIA source. Search, extraction, direct HTTPS media
fetch, MP4 demux and packet-copy MPEG-TS mux run on the HR54. The existing
playURL path configures the Broadcom H.264/AAC hardware decoder. There is no
host resolver, external API, remux server, transcoder, or software encoder.

## Runtime and current extraction findings

Versions inspected and tested on 2026-10-01:

| Component | Version | Proven receiver behavior |
|---|---|---|
| yt-dlp | 2026.08.19 (release published 2026-08-19) | Metadata, format enumeration, public playback, search |
| CPython | 3.14.0 | Static MIPS32 BE runtime, SSL, sockets, subprocess, zipimport |
| QuickJS | 2026-06-04 | Native execution, BigInt, typed arrays, EJS parser/library |
| yt-dlp EJS | 0.8.0, bundled in official yt-dlp archive | Loads on QuickJS; actual web challenge exceeded 60-second diagnostic limit |
| FFmpeg libraries | 8.0 | MOV demux, H.264/AAC packet copy, MPEG-TS mux |
| Private Python OpenSSL | 3.5.4 | SSL module and certificate verification |
| zlib | 1.3.1 | Compressed bytecode zip imports |

The actual pinned extractor defaults to `visionos,web`; MEDIA explicitly selects
only `visionos` to avoid adding the slow web challenge to public playback. The successful direct
AVC/AAC path uses the maintained visionos client anonymously. It does not
require a JavaScript challenge or PO token in the tested public-video path.
This is a supported client choice entirely inside yt-dlp on the receiver.
It is not a remote extraction service or an old extractor workaround.

Current [EJS documentation](https://github.com/yt-dlp/yt-dlp/wiki/EJS) describes
supported runtimes and bundled challenge components. The
[PO Token Guide](https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide) distinguishes
client-specific GVS/player/subtitle requirements. Some web clients require
PO tokens; that does not make every client require one. No PO provider,
browser stack, Node, Deno, or remote token service was installed.

Explicit `web_embedded` extraction reached player `8ab5c328-main` and the
QuickJS EJS path but exceeded 60 seconds. Parent peak RSS was 54,672 KiB;
parent CPU was 13.166 seconds. QuickJS samples separately reached 65,124 KiB
peak RSS and 38.53 CPU seconds before termination (sampled lower bounds, not
a complete wait4 child measurement). Thus real web signature/nsig challenge completion is
**not established**. Public visionos playback is established. If YouTube
removes that path, the web challenge performance becomes a concrete blocker;
it must not be concealed by moving extraction to another computer.

## Private cookies

At the user's later explicit request, the maintained Chromium cookie extractor
exported a domain-filtered Google/YouTube Netscape cookie jar. A Google account
check confirmed that the requested account was present without printing cookie
values. The exported file is outside the repository, installed as
`youtube/cookies.txt`, mode `0600`, inside a `0700` private runtime directory.
The original Chromium database was not modified.

`youtube/bin/yt-dlp` loads `youtube/yt-dlp.conf`, including this cookie jar and
the on-box QuickJS runtime. Search also loads the cookie jar. Public MEDIA
playback deliberately uses the measured anonymous visionos path, because
logged-in web extraction currently has the expensive challenge limitation
above. Installing cookies does not establish members-only, private,
age-restricted or otherwise authenticated playback. Cookies can expire or be
revoked; no automatic browser-cookie refresh or PC runtime dependency exists.

## Media pipeline and selection

```
YouTube ID / search
  -> native API -> on-demand Python + yt-dlp on HR54
  -> selected signed AVC and AAC URLs (private, ephemeral)
  -> existing hr54-iptv-fetch + CA bundle, two concurrent HTTPS workers
  -> bounded FIFOs -> minimal libavformat/libavcodec packet-copy helper
  -> receiver-local /youtube-stream/<opaque token>.ts
  -> existing playURL -> Broadcom decode -> HDMI
```

The selector requires AVC (`avc1`, `avc3`, H.264), width <=1920, height <=1080,
and a known positive video bitrate <=8,000 kbit/s. AAC-LC (`mp4a.40.2`) is
required; AV1, VP9, HEVC and HE-AAC are rejected. The highest compatible
resolution/frame rate is preferred, then AAC bitrate. Unknown bitrate is
rejected conservatively. Combined AVC/AAC MP4 is supported by feeding its
tracks to the same demux path; it currently makes two source requests.
Separate AVC MP4 and AAC M4A were proven end to end.

The minimal FFmpeg configuration disables every codec, encoder, network
protocol and unnecessary multimedia library, enabling only MOV demux, MPEG-TS
mux, H.264/AAC parsers, file/pipe and h264_mp4toannexb. The helper copies packets,
rescales timestamps and interleaves by DTS. The mux emits PAT/PMT and converts
length-prefixed AVC NALs to Annex B. AAC remains unchanged. No raw video/audio
frames or software codec conversion exists in this path.

Signed URLs are never durable state or TV API payloads. The backend accepts
only an 11-character video ID, owns URL resolution, and uses an unpredictable
single-claim local stream token. A failure before any TS bytes permits one
fresh resolution attempt. There is no unbounded retry loop or arbitrary URL
proxy endpoint. Search responses contain at most six compact results.

## Measurements and playback evidence

| Experiment | Elapsed | CPU | Peak RSS |
|---|---:|---:|---:|
| Local 18-second MP4 + M4A remux | 0.382 s | 0.368 s | 1,288 KiB |
| Paced streaming fixture remux | 18.028 s | 0.505 s | 1,324 KiB |
| QuickJS smoke | 0.004 s | 0.004 s | 892 KiB |
| EJS library/parser load | 0.276 s | 0.272 s | 2,664 KiB |
| Precompiled Python smoke | 0.856 s | 0.804 s | 8,488 KiB |
| yt-dlp version/startup | 4.679 s | 4.591 s | 19,868 KiB |
| Public metadata resolution | 15.380 s | 14.272 s | 37,400 KiB |
| Real YouTube streamed remux | 10.651 s | 0.171 s | 1,192 KiB |

Source-only Python imports took 8.172 seconds for a smoke test; source yt-dlp
startup exceeded 40 seconds. Portable CPython 3.14 bytecode zip imports are
therefore important to practical startup. No general package manager exists
on the receiver. Python/QuickJS consume no resident RAM while unused.

The local remux emitted 6,411,176 bytes, first packet after 0.028 seconds:
about 16.8 MB/s, approximately 47x media realtime. Paced first packet was
0.114 seconds. Video slice bytes matched after normalizing Annex B parameter
sets/AUD; AAC ADTS output matched exactly. See
`tests/youtube-20261001/elementary-stream-validation.json`.

The public test was `jNQXAC9IVRw`, **Me at the zoo**, duration 19 seconds.
Format 133: AVC 320x240/15 fps, approximately 183 kbit/s; format 140: AAC-LC,
approximately 130 kbit/s. Metadata resolution returned 24 formats and thumbnail
metadata without downloading media. The HR54 fetched 433,081 video bytes and
309,288 audio bytes directly over HTTPS; both fetchers exited successfully.
First TS packet was available after 1.001 seconds of media-worker execution.
The 19-second clip was remuxed in 10.651 seconds (~1.78x realtime), using
~1.6% of one CPU for the remux itself. Fetch-worker cost is separately visible
in the captured process samples; it is not included in that remux CPU number.

Receiver logs show MPEG-TS recognition, H.264 hardware decoder setup, AAC PID
recognition/configuration, notifyStarted, notifyFramePresented and advancing
position. Physical STOP restores `?source=youtube` and the saved search/page/
focus. No persistent decoder/no-frame error or software video encoder was
observed. Evidence includes `real-play-receiver.log`, `real-play-worker.log`,
`native-youtube.receiver.log`, `native-youtube-resources.json` and
`native-api-validation.log` under `tests/youtube-20261001/`.

A second ordinary public video, `aqz-KE-bpKQ` (**Big Buck Bunny 60fps 4K -
Official Blender Foundation Short Film**), resolved in 19.156 seconds and
played after reboot. This is the source title, not a claim of 4K output; the
AVC selector still caps output at 1080p. Across a 13-second steady interval,
all six fetch/relay/remux processes used 1.94 CPU seconds (~14.9% of one
core), including 0.54 remux CPU seconds. Their sampled aggregate RSS peaked
at 4,840 KiB. Receiver Ethernet receive throughput was approximately
4.49 Mbit/s (includes background receiver traffic). Hardware AAC and video
frames continued and playback position advanced. See `steady-playback-metrics.json`
and `post-reboot-bunny-*` evidence.

One post-reboot attempt with the explicit visionos+web combination exceeded
the resolver deadline and displayed a clean error. MEDIA now explicitly uses
visionos alone; the full physical TV flow passed with that configuration.

## API, state and lifecycle

- `GET /api/youtube/status`: runtime availability, active worker/playback, error.
- `GET /api/youtube/search?q=...&page=...`: six compact results, paging flag.
- `POST /api/youtube/play`, `{"videoId":"jNQXAC9IVRw"}`.
- `POST /api/youtube/stop`: cancel preparation or stock STOP active playback.
- `GET/POST /api/youtube/state`: separate query/page/index/selected-ID checkpoint.

The native service never runs extraction synchronously in its main listener.
Each request uses a controlled process group, cancellation polling, bounded
JSON, a 60-second resolver limit, and exact group teardown. Resolver address
space is limited to 128 MiB and CPU to 50 seconds. Media pipes have bounded
buffers, 25-second inactivity detection and socket send timeouts. STOP and
BACK cancel workers, reap children, remove session FIFOs and preserve other
source state. Returning to results performs a new compact search on the HR54;
this short-lived search worker is expected, not an orphan playback worker.

Initial controls are STOP and BACK. API pause/resume/seek are not implemented
for YouTube. Physical firmware controls have not been established as a reliable
YouTube pause/seek implementation. Thumbnails are omitted from the TV UI.

## Automatic updates

The existing native MENU watcher checks once per minute while idle. If the
update status file is at least seven days old, it launches the updater on-box.
There is no host cron job, external updater, package manager or resident Python.
The updater uses the existing verified HTTPS helper to read the official
GitHub latest release and fetch `yt-dlp` plus `SHA2-256SUMS`.

It enforces download/expanded archive bounds, rejects traversal, validates the
release SHA-256, compiles source modules into portable bytecode **on the HR54**,
checks the staged runtime's version, and atomically replaces the archive under
an exclusive lock. Resolver readers retain a shared lock. A previous archive
and version are kept; failure preserves the current runtime and records an
error in `youtube/update-status.json`. It runs at nice 15 with a 160 MiB address
space limit, 300 CPU-second limit and 360-second native wall limit. It checks
for active sources during preparation and before commit. A separate updater
lock prevents concurrent update preparation; stale interrupted staging
directories are cleaned on the next attempt. The attempt is timestamped before
heavy work, so forced termination cannot create a one-minute retry loop. A failed scheduled
attempt is retried at the next weekly interval. EJS assets update with the
official yt-dlp archive; Python and QuickJS remain pinned and need deliberate
compatibility upgrades if a future yt-dlp changes their requirements.

The full update path was tested by temporarily marking the installed version
as older, then fetching and rebuilding the current official release on-box.
It completed in 83.791 seconds, used 78.690 CPU seconds and peaked at
38,736 KiB RSS. The actual archive passed version and subsequent hardware
playback tests. This validates download, checksum, compilation and atomic
replacement, not only a no-op latest-version check. Runtime disk footprint
including the previous archive is 32,996 KiB.

## Build, deployment and rollback

Sources/build settings are in `youtube/build-remux.sh`, `build-quickjs.sh`,
`build-python.sh`, `build-native.sh` and `package-python.py`. Toolchain:
`/tmp/hr54-zig/zig`, target `mips-linux-musleabi -mcpu=mips32`, static musl
helpers. Existing uClibc/curl/wolfSSL HTTPS adapter is reused unchanged.
Private OpenSSL is needed only for CPython's SSL module; the stock adapter does
not expose CPython's required OpenSSL API. No second network relay was written.

Build scratch is `/tmp/hr54-youtube-build`. CPython required a small atomic
lock-free query shim and deduplication of statically linked HACL objects; these
are recorded in the build scripts. QuickJS host-generated REPL bytecode uses
`host-qjsc -x -s` for big-endian MIPS. CPython zip bytecode magic is `2b0e0d0a`.
Artifact/source checksums and compiler logs are in the evidence directory.

For small helper changes, run `sh youtube/build-native.sh` and
`python3 youtube/stage-helpers.py --receiver 192.168.88.103` from `jellyfin/`.
The helper deployment checks production hashes and idle state, retains a
private backup, verifies `.next` hashes and swaps atomically. The final private
helper receipt is `tests/youtube-20261001/helpers-final-deployment.json`.
`stage-runtime.py` installs/reinstalls the complete pinned runtime and accepts
an optional private `--cookies-file`; it preserves existing cookies when
none is supplied. The one-time PC export was removed after installation and
post-reboot validation.

Runtime files live only under `/var/hr54-persist/jellyfin/youtube/`; session
FIFOs use `/tmp/hr54-youtube-<worker PID>`. Static binary sizes: remux 1.44 MB,
QuickJS 1.65 MB, Python 14.16 MB; precompiled stdlib 4.73 MB and yt-dlp 5.80 MB.
No stock rootfs, flash, Wi-Fi NVRAM, plugin or boot image was changed.
Existing boot supervision starts the same native service, which uses these
persistent on-demand helpers without manual post-reboot setup.

Deployment verified receiver identity, idle state and production hashes;
staged and hashed `.next` files; retained persistent backups; synced and renamed
atomically; stopped only verified listener/child PIDs. See production deployment
logs. The backup `backup/iptv-20261001-125344` restores the original three-source
backend and TV assets. The later `backup/iptv-20261001-130152` restores the first
YouTube integration. Restore only backend/app.js/app.css/index.html using the
same idle/hash/stage/sync/rename procedure, then restart exact service PIDs.
Jellyfin tokens, IPTV state, Frigate config and YouTube cookies need no rollback.
To roll back a yt-dlp update, stage `yt-dlp.previous.zip` and `version.previous`
as `.next`, test the staged version, then swap while all resolvers are idle.

## Final regression and boot evidence

A controlled reboot returned the same receiver MAC/address with native service
startup, Jellyfin authentication, all source checkpoints and private cookies
retained. `reboot-before.json`, `reboot-after.json` and `reboot-startup.log`
record the checks. The native scheduled updater also performed a latest-version
check automatically; its status changed to `current` after the deliberately
aged scheduling timestamp.

Live post-reboot regressions passed Frigate discovery and camera frames;
Jellyfin authentication/browse/playback/pause/resume/seek/STOP; MENU/MEDIA and
BACK/EXIT; and existing IPTV groups/search plus native verified HTTPS HLS with
H.264 frames and AAC hardware configuration. IPTV search/page/focus and other
source checkpoints were preserved; temporary test checkpoints were restored.
See `post-reboot-source-regression.log` and `post-reboot-iptv-validation.log`.

Doom remained unarmed, as it was before this work. Its existing engine/assets
and arming state were preserved. The unchanged frontend decoder regression
passed all pixel/sequence/malformed-frame checks; no live game launch is claimed
while its existing arming flag is absent.

Additional native tests exercise responsive service behavior during resolution,
worker exclusivity, cancellation, independent state, ID-only inputs and orphan
teardown. Fault-injected feeder/remux fork failures verify that negative PIDs
can never become signal targets and session FIFOs are removed.

The final post-reboot physical flow passed MENU → fourth source → remote
keyboard → on-box search → physical result selection → AVC/AAC hardware
playback → STOP → the same search/page/focus. Evidence:
`post-reboot-youtube-tv-validation.log`, `post-reboot-youtube.receiver.log`
and `post-reboot-youtube.return.log`. The receiver was left idle with the
original Jellyfin/IPTV checkpoints preserved.
