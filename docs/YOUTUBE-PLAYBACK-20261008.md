# YouTube delivery, backpressure and decoder drain

The affected 248-second video repeatedly stalled in both audio and video and
could return to Home before its buffered tail finished. Receiver CPU usage was
low. Full-file CDN delivery took up to 310 seconds, and a bounded 1 MiB request
on the same receiver was more than four times faster. Formats remain unchanged:
H.264 format 137 and AAC format 140. No quality reduction or transcoding was added.

The relay now requests verified contiguous 1 MiB Googlevideo ranges, prebuffers
1 MiB video / 64 KiB audio, and maintains independent bounded 2 MiB queues per
track. Unknown CDNs retain the whole-file fallback. A short/oversized chunk
fails rather than silently skipping or duplicating bytes.

Module and core streaming writes now wait through decoder backpressure with
cancellable polling instead of treating the ordinary 3/5-second socket send
timeouts as playback failure. No-progress deadlines remain bounded; Stop and
session replacement interrupt these waits. HTTP request timeouts are unchanged.

EOF is sent to the decoder immediately, while module playback remains active
through at least a 17-second drain window after EOF. A late download can no
longer expire that window before its last bytes arrive.

## Validation

The full backend suite passed, including seven YouTube integration tests and
four tests of the actual production relay. These cover range reconstruction,
whole-file fallback, truncated chunks, cancellation while prefilling, a decoder
not reading for six seconds, and EOF arriving after the old deadline expired.

Two consecutive receiver replays were monitored in logs without a viewer:

- Both retained normal decoder rate 1000 through the video.
- Decoder rate reached 0 around 249 seconds, before module playback ended.
- Both video/audio feeders delivered their complete 83,013,683 / 4,017,956 bytes.
- Each remux completed all 16,894 input packets / 86,904,962 media payload bytes.
- Both relays exited cleanly; no transfer failure or early forced decoder stop.

These are log-based playback/transport results, not a fresh visual/audio
observation. The update is installed at 192.168.88.103. Original receiver
binaries are preserved under
`/var/hr54-persist/backup/youtube-stutter-20261008/previous/`.
The rebuilt core and YouTube module/relay are saved in `receiver/runtime/`.
The module changes persist across normal restart; no middleware/firmware or
account/playlist changes were made.
