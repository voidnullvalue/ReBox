# Runtime backend upgrade: 2026-10-07

The existing HR54 installation at 192.168.88.103:8130 now runs `reboxd`
and the generic native UI. This records a live upgrade of an older manual
installation, not acceptance of the general installer or a reboot.

## Observed results

- The new daemon first ran on loopback port 8132. The native CLI readiness/idle
  check passed; all five installed/enabled built-in modules reported healthy.
  Jellyfin reported configured/authenticated using the existing private data.
- The guarded activation finished with exit code 0. The public port 8130
  reported core readiness, empty `nativeModule`, and `mediaBusy=false`.
- The user explicitly confirmed native Home and receiver remote navigation.
- The installed Android app connected over Wi-Fi. Its captured Home displayed
  five runtime module cards and icons; the user confirmed the phone screen.
- Guarded rollback finished with exit code 0. Restored backend/UI hashes matched
  the snapshot, the old API reported ready, and the user confirmed the old
  Home screen and remote navigation.
- Reactivation finished with exit code 0. All five modules reported healthy;
  the user confirmed the new Home and receiver remote navigation again.
- The boot UUID stayed unchanged. Bootstrap logs recorded an already-loaded
  policy rejoin rather than a middleware reload. The existing input broker
  PID 6033 survived both switches and rollback.

## Recovery

The verified, private snapshot is:

`/var/hr54-persist/rebox-upgrades/runtime-20261007-v2/backup`

The receiver-local pointer is `/var/hr54-persist/rebox-runtime-rollback-path`.
When media/native apps are idle, invoke through the trusted root shell:

```sh
/var/hr54-persist/rebox-runtime-rollback.sh RESTORE_PRE_MODULE_BACKEND
```

Run long operations detached with a durable log; the shell transport's drain
window must not terminate the operation. The script checks receipt integrity,
loaded policy, media idleness and process executable identity, then stops the
owned frontend/core and restores the saved code. It retains accounts, playlists,
module data and packages. The incomplete earlier `runtime-20261007/backup`
directory is not a recovery snapshot.

The immutable older boot hook still invokes the legacy backend filename. A
small compatibility launcher at that path forwards to generic ReBox startup;
rollback restores the original executable. Stock boot/plugin files, native
policy, input broker and middleware were not replaced.

The transferred archive was SHA-256:
`be122e06d6dfe099b3b648fb8d86bae58e95a5201f2da284509f6653aa86d2f9`.
Its original SHA helper required unavailable `statx`; before activation it was
replaced with the Linux-3.3-compatible `fstat` build. The archive hash therefore
is not a hash of the final installed tree. The final tree and staged activation
scripts passed the separate receiver-local `activation-files.md5` receipt.

## Still requiring acceptance

Real playback/video/audio, pause/resume/stop and return-to-Home for Jellyfin,
IPTV, YouTube and Frigate; Doom surface/input handoff and restoration; Android
pairing and privileged module management; arbitrary package install on this
receiver; complete remote key coverage; restart, warm reboot and cold boot.
Doom sound remains a known broken feature. A healthy module process does not
prove server connectivity or decoder compatibility.
