# ReBox — HR54-700 native media shell and Android controller

This is a self-contained installation/development bundle assembled on
2026-10-04 from the physically accepted repaired receiver deployment.
Start with [the stock installation guide](docs/STOCK-INSTALL.md), not the
historical installation scripts inside `source/`.

**Not a universal firmware image or a one-click stock network jailbreak.**
The proven stock entry requires powered-down HDD access. The supplied native
binaries/policy are restricted to one exact vendor build. Model number alone
does not prove compatibility. Unsupported firmware/geometry must fail closed.

| Path | Contents |
| --- | --- |
| `android/ReBox-controller.apk` | Exact repaired APK installed on the Pixel 6 |
| `receiver/payload/hr54-persist/` | Exact repaired native UI, broker, API, playURL wrapper; Doom, IPTV TLS, YouTube runtime; no accounts |
| `stock-bootstrap/` | Portable asset-7 root boot hook, genuine anchor, big-endian MIPS helper kernel/modules/initramfs |
| `tools/prepare-stock-image.sh` | Offline installer for a regular **cloned** `/var` partition image; never physical block devices |
| `tools/activate-native.sh` | Receiver-side exact-firmware checks, backup, native activation |
| `tools/deactivate-native.sh` | Receiver-side backed-up stock presentation restore, applied by reboot |
| `tools/recv/` | Existing root-shell/file-transfer helpers; set `HR54_HOST` to your receiver |
| `source/hr54-re/` | Current receiver/UI/broker/backend/Doom source, build scripts, minimal vendor build sysroot |
| `source/android/` | Android Gradle project including vendored Whisper and voice model; no signing key/cache |
| `SHA256SUMS` | Integrity manifest for all public bundle files |

Native mode retains the vendor media/compositor services, suppresses Druid,
restores the vendor RF radio independently, and uses DirectTest/playURL.
It does not replace the kernel, write flash, decrypt recordings, factory reset
the box, reset remote pairing, or change the receiver network configuration.

Read these before writing any disk:

- [Stock install](docs/STOCK-INSTALL.md)
- [Security and privacy](docs/SECURITY.md)
- [Rollback](docs/ROLLBACK.md)
- [Android installation](docs/ANDROID.md)
- [Physical/API acceptance](docs/ACCEPTANCE.md)
- [Build and limitations](docs/BUILD-AND-LIMITS.md)
- [Package validation](docs/VALIDATION.md)

Verify from this directory: `sha256sum -c SHA256SUMS`.
Configure your own Jellyfin server; log in with your own account after activation.
The IPTV playlist is intentionally empty. YouTube cookies, Jellyfin tokens,
receiver/device keys, Wi-Fi state, phone settings and recordings are NOT included.

Validation boundary: repaired core binaries were accepted with physical input,
HDMI video/audio, pause/resume, replacement playback and stop-to-Home on one
HR54-700. Warm reboot returned to Home with physical RIGHT traced. The new
portable stock packaging/installer has offline checks, but has **not** been
installed onto a second stock physical receiver. Do not call a new installation
working until its own physical/HDMI acceptance passes.
