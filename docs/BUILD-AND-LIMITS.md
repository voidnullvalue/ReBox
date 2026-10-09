# Build, provenance and known limits

The authoritative repaired receiver source was `/home/code/hr54-repair`, not the
older main worktree. The packaged backend is that tree's `jellyfin/remote/hr54-jf`,
NOT its stale `remote/bin/hr54-jf`. The packaged CAR was retrieved from the live
receiver and independently regenerated with the hash-guarded source policy tool;
it releases only GUIDE/MENU/LIST/EXIT. The older build directory policy was not
used. The bundled APK is the original accepted build, not the rejected re-signed
APK from the repair evidence directory.

| Accepted receiver component | MD5 |
| --- | --- |
| hr54-ui | e014f494fa7a056666f4709c09632419 |
| hr54-input-broker | 5524b8e01569cf8e66b7586a03874eba |
| hr54-jf | c929dc866437cae707e1e0546650f523 |
| hr54-play-url | 853fe5b3c7c6e0a61160af438b6cece0 |
| CAR ownership policy | c1b9e5ba4546c42f799cf580f048afb1 |
| native Doom | dc8d44e254cedc79e39c21036d36eedd |

The new portable plugin indexer preserves the exact genuine indexer/library and
root listener from the proven asset-7 hook but uses `rebox.conf` and private-LAN
firewall ranges instead of the original fixed Jellyfin/LAN addresses. It omits
old stock artwork, wording, settings, ITV overlays and asset-23 customization,
which the native shell does not need. It is a newly built package, not the old
deployment plugin hash; inspect `SHA256SUMS` for its identity. No native policy
guards or media-parser checks were weakened.

## Receiver rebuild

Prebuilt binaries suffice for installation. For rebuilding, install a compatible
Zig toolchain, C compiler, Python 3 and (host tests) Pillow. The original repair
used `/tmp/hr54-zig/zig`; set paths for YOUR machine. The minimal vendor link
sysroot is under `source/hr54-re/extracted/sdb4-rootfs`.

```sh
cd source/hr54-re
export HR54_ZIG=/absolute/path/to/zig
export HR54_SYSROOT="$PWD/extracted/sdb4-rootfs"
make -C hr54-ui receiver
HR54_TEST_POLICY="$(cd ../.. && pwd)/receiver/payload/hr54-persist/native-menu/dtv-menu-policy.car" make -C hr54-ui test
"$HR54_ZIG" cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
  -o /your/output/hr54-jf jellyfin/remote/hr54_jf.c
OUT=/your/output/hr54-doom-native sh doom/tools/build-engine.sh --native
python3 hr54-ui/tools/menu-policy.py extracted/sdb4-rootfs/opt/dtv/dtv.car /your/new/policy.car
```

MIPS is **big-endian MIPS32/o32**, not MIPS32r2, MIPS64 or mipsel. Native graphics
uses the exact vendor uClibc/EGL ABI, not arbitrary host libc structs.
Rebuild output may differ with toolchains; update manifests deliberately and
repeat host and physical/API acceptance. Do not deploy by copying an older
`build/` binary just because source is newer.
The bundled test harness accepts an explicit policy fixture and creates the
matching bootstrap digest; backend tests use a new temporary directory rather
than deleting a hardcoded historical directory. These are packaging-only test
portability changes, not changes to the accepted runtime binaries.

Frigate currently uses compile-time `FRIGATE_HOST="192.168.88.39"` and
`FRIGATE_PORT=5000`. To change it, add e.g.
`-DFRIGATE_HOST='"YOUR_FRIGATE_IPV4"' -DFRIGATE_PORT=5000` to the backend build.
No authenticated Frigate server integration is established here. This package
does not change the receiver or another server's network to match old addresses.

## Android rebuild

`source/android/` includes Gradle wrapper, Kotlin/Compose code, Whisper C++ and
the bundled model. Use JDK 17, Android SDK 37, NDK 28.2.13676358, CMake 3.22.1,
and your SDK path in a new `local.properties` or `ANDROID_HOME`.
Run `./gradlew :app:assembleDebug`. Dependency downloads need Internet access.
No signing private key is supplied. Your rebuilt signature may require a fresh
install; preserve user data/permission decisions.

## Root plugin and helper

`stock-bootstrap/plugin-root` is the staged plugin tree; `stock-bootstrap/indexer`
is its portable launcher source. To rebuild after changing the launcher:

```sh
cp stock-bootstrap/indexer stock-bootstrap/plugin-root/mp4lib/bin/indexer
chmod 700 stock-bootstrap/plugin-root/mp4lib/bin/indexer
mksquashfs stock-bootstrap/plugin-root /your/new/7_6933_6840.squashfs \
  -noappend -comp lzma -all-root -mkfs-time 1722966254
```

Do not change the signature's genuine IMAGE anchor or assume this gives a new
vendor signature. `tools/plugin_verify.py` checks the genuine anchor using
the bundled exact vendor verifier public material. The big-endian helper uses
Debian 4.19.0-21-4kc-malta kernel/initramfs and exact matching XFS/virtio modules;
not your host kernel modules. Debian copyright notice is included.

## Third-party runtime/source

YouTube runtime: receiver-native CPython 3.14, QuickJS, yt-dlp and packet-copy
FFmpeg helpers. Public CA data included; cookies intentionally omitted.
Repaired native mode does not require SHEF port 8080. Source scripts are in
`source/hr54-re/jellyfin/youtube`; their historical build staging paths may need
adjustment on another workstation. FFmpeg source archive is included.
The bundle is an install snapshot, not a complete hermetic rebuild of every
third-party dependency. Obtain matching upstream CPython/OpenSSL/zlib/QuickJS/
yt-dlp sources if rebuilding those helpers. Fonts/icon, Doom/Whisper and Debian
notices are retained; vendor redistribution rights are not asserted.

## Known hardware/functional limits

- The new portable installer passed a stock-image read-only audit and complete
  guest install/read-only verification on a disposable copy-on-write overlay
  backed by the original stock HDD image. It is not yet accepted on a second
  physical stock receiver; image checks do not substitute for HDMI/RF acceptance.
- Other firmware/partition geometries are unsupported and deliberately rejected.
- Before this bundle, repaired core was physically accepted for navigation,
  Jellyfin HDMI video/audio, MENU/EXIT, combined pause/resume, replacement play
  and stop-to-Home. Warm reboot Home/RIGHT was accepted; post-reboot playback,
  cold power cycle, IR/front panel, held-repeat feel and every optional source
  were not reaccepted after final repair.
- Distinct GUIDE e00b and dedicated PLAY e400 were not proven by this remote;
  the GUIDE-labelled exercise emitted MENU e503 and combined playback e401.
- Native output reported 720x480 after warm boot versus 1920x1080 during final
  live acceptance. Full-width layout adapts to the vendor output; this is not
  a promise of persistent 1080p output on a stock boot. Inspect HDMI mode.
- IPTV needs a user playlist and receiver-compatible H.264/AAC/AC-3 media.
  MPEG-TS can pass through, and unencrypted H.264/AAC or H.264/AC-3 fMP4 HLS
  with in-band audio can be packet-copied to MPEG-TS. Separate HLS audio
  renditions, byte-range HLS, arbitrary DASH/DRM, and unsupported codecs are
  not converted by this shell; see `docs/IPTV-COMPATIBILITY-20261008.md`.
- YouTube online extraction can change. This is the known working snapshot,
  not a promise that all current/private/age-gated videos work. No cookies or
  external PC resolver supplied; public anonymous path is the intended default.
- Doom shareware notices and engine source are supplied; no commercial full
  game WAD. Native Doom-specific aspect handling is kept separate from Home.
- The debug Android APK includes arm64 only and is not signed by a publisher
  release key. Actual phone-originated playback must be checked on your device.
