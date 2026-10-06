# ReBox Android controller

The controller requires the runtime module API (`moduleApi: 1`). It discovers
Home cards from `GET /api/modules`; services are not APK destinations. Home and
Settings remain fixed navigation entries. An arbitrary module ID can provide
folders, search results, artwork, playback, settings and actions without a new
APK. Disabled and uninstalled bundled modules remain in the manager.

The browser keeps one bounded page (60 rows), 12 cursor/history entries and a
bounded window of previous page offsets. Search input is 64 UTF-8 bytes; install
URLs use a separate 1,024-byte input. Artwork uses module proxy URLs, including
percent-encoded opaque IDs and a generic missing-image fallback. Playback uses
runtime source strings, transport capabilities and the core instance/generation.

## Module management

On the HR54, open **Settings → Modules → Pair Android management**. Enter the
receiver's temporary code in Android **Settings → Modules**. The controller
exchanges it for its own bearer; it never reads receiver-local secrets.

Bearers are scoped to normalized receiver addresses, written atomically with
mode 0600 in app-private `noBackupFilesDir`, and excluded from backup/transfer.
A rejected bearer clears local pairing. Enable/disable, uninstall, offline
bundled reinstall, URL install, settings and actions require pairing. Registry
refresh after a mutation updates Home without reconnecting. Uninstall keeps
receiver module data. Third-party packages execute receiver software and must
come from trusted sources.

Native-app controls use manifest presentation flags. Apps requiring display or
input release launch through the HR54 shell, which owns that handoff; Android
can stop them. Apps requiring neither release can use generic remote launch.
The current core does not coordinate a remote exclusive handoff, so the Android
client does not bypass it. Receiver boot and presentation safeguards are unchanged.

## Build and verification

With Android SDK 37, NDK 28.2.13676358, CMake 3.22.1 and Java 17 or newer:

```sh
cd source/android
./gradlew :app:testDebugUnitTest :app:assembleDebug
./gradlew :app:compileDebugAndroidTestKotlin
```

The APK is `app/build/outputs/apk/debug/app-debug.apk`. It contains the existing
ARM64 local Whisper implementation/model; audio and transcripts stay on-device.

Tests cover runtime models and bounds, zero/32 modules, opaque folders and
pagination, search-first modules, cancelled replies, decoder instance changes,
serialized transport, management authorization, receiver-scoped token storage,
settings/actions and Compose UI behavior. The real-core integration fixture
builds a host daemon/module, generates a package ID after client compilation,
and exercises pairing/install/browse/search/play/disable/enable/uninstall through
the production Android HTTP client and view model. Only the receiver decoder is
mocked. The fixture reads a local management secret only to simulate the trusted
HR54 pairing UI, and passes Android only a temporary code.

Compose screenshots are generated under ignored `docs/previews/`. These are host
renders, not phone or HR54 acceptance. Instrumented UI tests must still run on
an Android device/emulator. Android 37 LAN permission, microphone permission,
local speech, real receiver media playback and native presentation require
physical acceptance. Production receiver packaging/startup is a separate checkpoint.

In a shared build environment, use writable `ANDROID_USER_HOME`,
`GRADLE_USER_HOME` and `CCACHE_DIR`. Reuse the existing debug keystore in the
selected Android user directory to preserve debug upgrade signatures. Offline
builds require the SDK and dependencies to be cached.
