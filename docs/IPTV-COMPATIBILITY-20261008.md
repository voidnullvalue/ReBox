# IPTV compatibility investigation — 2026-10-08

## Runtime and playlist

The development receiver at `192.168.88.103` is a BCM7346B2 HR54 running
big-endian MIPS32 Linux. Its installed IPTV module and this checkout's original
binary both had MD5 `e5d9e2bd6699ce8ebe829338060a2fad`. Core `reboxd`
owns playback sessions and the vendor player; the IPTV module owns the playlist,
HTTP fetch, rendition decisions, and TS relay. No core playback changes were
needed. The separate `/home/void/vroomfondle/dvr/ReBox` checkout is older and
contains unrelated local edits; this work uses `/home/void/ReBox` on `main`.

The receiver playlist is still
`/var/hr54-persist/jellyfin/iptv/eng.m3u` (476,555 bytes, 2,096 entries).
Its SHA-256 at investigation time was
`4c93466df83d23e7faa6f84cb2175fbce00d4b9d8819e993cc39c6931f2ae360`.
It has 2,058 `.m3u8` URLs, ten `.mpd` URLs, one SRT URL, and 27 URLs with
no extension. Do not commit the receiver playlist or diagnostic URLs: signed
queries and private provider information may be present.

## Failure evidence

`tools/iptv-scan.py` samples channels sequentially with one worker, a bounded
response, six-second request timeout, and at least 0.2 seconds between
channels. It reports names and categories without URLs or query tokens. Its
`media_verified` result requires an FFprobe video track and a compatible
codec candidate in sampled media; it does **not** establish physical playback.

On a 72-channel evenly spaced host sample: 27 yielded H.264 media candidates;
26 returned HTTP 403, five 404, one 5xx, two other HTTP errors, three connection
failures, one TLS failure, and one DNS failure. One returned HTML instead of
media. One advertised encrypted HLS, one was DASH, one advertised no H.264
rendition, one had MP3 audio, and one returned an unknown direct container.
403 responses alone do not prove a geographic restriction; headers, tokens,
or provider policy may cause them. All 27 media candidates in this sample
used MPEG-TS HLS on recheck. Host network reachability may differ from the
receiver's.

The old relay had no fMP4 handling and only recognized a PMT if the entire
section fit in one TS packet. It selected a single HLS rendition from
bandwidth/manifest text without checking a segment. Direct TS checked a tiny
startup buffer. Live HLS exited on a single refresh or segment fetch error.
The fetch adapter hid HTTP status from the module, so users saw a generic
network failure for 403/404/429/5xx.

FilmBox is a direct H.264/AC-3 transport stream. Its first PMT arrived at
byte 44,932, beyond the old 32 KiB startup read. A 20-second host capture
had no TS continuity or PCR errors, but began mid-GOP and FFprobe reported
missing H.264 parameter sets until the first decodable keyframe around
byte 663,452. The initial receiver run showed picture and sound with startup
stutter. After a bounded 1 MiB startup buffer, the receiver owner confirmed
moving picture and audible sound with no startup or sustained stutter.

## Implementation

The module now assembles bounded, continuity-checked PMT sections across TS
packets and buffers up to 1 MiB of direct TS startup (five-second target).
HLS preparation fetches
and inspects representative media, tries up to three advertised compatible
variants, and logs redacted rejection reasons. Selected playlist URLs remain
the stable input to the next fetch so redirects can acquire fresh signed
children. HLS media parsing tracks initialization maps per segment and handles
map changes and discontinuities. The new IPTV-owned FFmpeg worker copies
H.264/AAC or H.264/AC-3 fragmented MP4 packets into MPEG-TS; it has no
networking, decoders, or encoders. TS HLS remains packet passthrough.

HTTP and curl failures now have safe status/error text. Live refreshes,
segments, and direct TS reconnections have three attempts, short backoff, and
session-aware cancellation. Successful segments clear the failure budget.
Temporary interruption appears in IPTV status as reconnecting. The existing
core remains the sole owner of decoder arbitration and `playURL`.
After a failed play request, the generic native UI reads the module's status
and displays its bounded diagnostic. It rejects URL-like status text; the
IPTV module owns the actual error wording. The UI does not resolve playlists
or make rendition decisions.

Byte-range HLS remains explicitly rejected because its offset/length state
and Range request propagation have not been implemented. Separate alternate
audio renditions are not combined with video yet; variants whose tested video
segment has no in-band audio are rejected with an explicit diagnostic rather
than silently played without sound. DASH has no receiver-native
manifest/segment path in this module. Encryption and DRM remain unsupported;
no bypass is attempted. HEVC, VP9, AV1, and unconfirmed TS audio types are
rejected or reported as incompatible. fMP4 code covers unencrypted H.264 with
AAC/AC-3 inside the same media rendition. Four accessible fMP4 channels in a
150-manifest sample (Access Tuolumne, BX Inform, Montgomery Channel, and
Qausain TV) all had separate audio groups, so they remain unsupported
as complete audio/video services. The scanner reports this separately from a
playable media candidate.

## Validation

The host FFmpeg fixture generated real 640×360 H.264/AAC fragmented MP4.
Host and big-endian MIPS32/QEMU remux outputs were byte-identical MPEG-TS with
both tracks. The staged worker also ran on the physical receiver against that
fixture; its output was pulled back and FFprobe found H.264 and AAC. These are
packet-copy checks, not visible/audible decoder acceptance. The IPTV regression
suite now covers fMP4 packet copy, split PMT, relative redirect resolution,
rendition fallback, transient 503 recovery, and STOP during recovery in
addition to its existing parser, TS HLS, cancellation, surfing, and malformed
format cases.

Receiver backup path: `/var/hr54-persist/backup/iptv-overhaul-20261008/`.
`previous/` retains the pre-update module, fetch adapter, and manifest;
`stage/` holds hash-checked replacement binaries and packet-copy fixtures.

On the physical HR54, AL24 News and Al Jazeera English each ran beyond 20
seconds with advancing segments; the receiver owner confirmed moving HDMI
picture and audible audio. CH+ skipped one unavailable neighbor without
destroying AL24, then reached Al Jazeera. STOP ended playback on HDMI with no
remaining fetch/remux child. FilmBox ran for multiple minutes; the owner
confirmed picture and audio without stutter after the startup buffer change.
A LAN-served 640×360 H.264/AAC fMP4 HLS fixture was remuxed at live pace;
the owner confirmed its moving picture and audible tone on HDMI. This validates
the remux path, but no public fMP4 service with in-band audio was accessible
in the sample.

During the paced fMP4 fixture, the IPTV module used about 1.3 MiB RSS
(2.1 MiB peak), and the remux child used about 1.2 MiB RSS (1.5 MiB peak).
Over five seconds, the module consumed five CPU ticks and the remux child six;
with 100 ticks per second this is about 1% and 1.2% of one CPU respectively.
The 66 backend tests, the full HR54 UI suite, and the module integration
suite passed. The receiver playlist is restored to its original MD5 after the temporary
fixture test. All six modules are healthy; the pre-test Frigate camera view
was restored and its HDMI picture was confirmed by the receiver owner.

The deployed IPTV module is version 0.1.2. Its final on-receiver MD5 is
`87e2c82da5d37782aa6d394554ab6403`; the UI MD5 is
`b286c5ae90c3b1650aeb4f077d0ff6b5`. The original UI and IPTV binaries
remain in `previous/` under the receiver backup directory. The original
2,096-channel playlist was restored with MD5
`7278fcda88bd83ad55bab06df3ac9fea`.
