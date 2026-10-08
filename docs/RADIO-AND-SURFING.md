# Internet Radio and fullscreen channel surfing — October 8

Internet Radio is a runtime media module. It imports 443 station entries across
16 genres from recommended-radio-streams, pinned at upstream commit
`8f07b17a8de3acd3c95c9dd6bc192b551634804c`. Browse all stations, choose a genre,
or search. The source catalog, upstream CC0 license, and file hashes ship with
the module.

During IPTV or radio playback, the RF remote's **CH+ / CH−** changes the station
without opening a menu. The screen immediately displays **Changing channel /
Please wait…**. Successful changes briefly show the new title. Unavailable
candidates leave the original stream playing; another press advances past the
failed candidate. Channels follow the module's full playlist order and wrap.
STOP cancels playback or a pending station check. Held repeats are suppressed
while a change is pending. Android exposes the same controls when the active
module advertises them.

The core sends an opaque current item and direction to the provider's private
`POST /adjacent` RPC. The provider selects and checks the next item; core owns
decoder replacement. `/api/playback/channelUp` and `channelDown` use `{}` bodies.
Plans and `/api/state` expose optional `transport.channelUp` / `channelDown`
booleans. The core and UI do not contain provider channel lists.

## Audio implementation and measured failure

The original MP3 packet-copy path was silent on this receiver. MP3-to-AAC
conversion produced audible audio but could not keep up. A 9.382-second actual
Ambinature MP3 sample took **18.74 seconds** to process on the receiver, using
17.99 CPU seconds. Fixed-point AC-3 at a forced 32 kHz improved that to 4.66
seconds, but stage measurements showed unnecessary sample-rate conversion was
still the dominant cost.

The final worker copies compatible AAC-LC at native rates. For MP3 and other
supported audio, it uses integer AC-3 encoding and retains supported source
rates (32/44.1/48 kHz). The same real MP3 sample now processes in **1.20 seconds**
on the receiver, with 1.14 CPU seconds: decode 0.417 s, resampling 0.009 s,
encoding 0.510 s, and muxing 0.033 s. Generated audio was decoded and verified
nonzero; FFprobe identifies AC-3 stereo at 44.1 kHz and baseline H.264.

A small pre-encoded black AVC track is looped for compatibility with the stock
TV media path; video is never encoded during playback. Live output is paced to
roughly one second ahead of wall time. Socket writes tolerate bounded decoder
backpressure, and Python forwards available HTTP bytes rather than waiting for
32 KB chunks. Worker shutdown terminates and reaps its encoder.

Live Ambinature measurements showed approximately **11% of one CPU core** for
the audio worker, about **3 MB resident memory** (3.8 MB high-water mark), and
approximately 14 MB for Python. Production remained about 1.03 seconds ahead of
wall time, with approximately 40 KB/s compressed input and 36 KB/s TS output.
The user confirmed audible radio without stuttering. The scheduled ten-minute
test was stopped at the user's request after that confirmation; it was not
completed and is not claimed as a ten-minute acceptance.

## Acceptance and remaining boundaries

- All 61 backend tests pass, including Jellyfin, IPTV, radio/surfing, YouTube,
  its production relay, Frigate, native apps, installer and registry tests.
- Native UI/reactor/artwork/input/rollback tests pass, including the wait
  overlay, late-response navigation preservation and CH key decoding.
- All 55 Android host tests pass; the rebuilt APK retains the existing bundled
  signing certificate.
- Real receiver IPTV testing changed between Al Jazeera English and AL24 News,
  including unavailable-neighbor preservation and retry. Radio RF traces show
  `e006` / `e007` reaching the fullscreen controller as CH+/CH−.
- STOP leaves no radio Python/encoder workers. Deployed binaries are rebuilt
  from the actual repository sources and verified against the release manifest.

Decoder startup and API success alone are not listening acceptance. The radio
listening confirmation above came from the user. Not all external stations were
probed, and another receiver/firmware has not been accepted. HTTP/HTTPS direct
MP3/AAC/Ogg/FLAC, PLS/M3U indirection and unencrypted MPEG-TS HLS are supported;
encrypted, fragmented-MP4 and byte-range HLS are rejected. The fetcher uses the
bundled receiver Python runtime seeded by the standard distribution.

Backups are on the development receiver under
`/var/hr54-persist/backup/radio-surf-20261008/`,
`radio-surf-verified-20261008/`, and `radio-main-release-20261008/`. Existing
accounts, IPTV playlist, boot scripts, firmware/policy and other provider
configuration were preserved.
