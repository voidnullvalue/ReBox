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

## Jeopardy phone controller (embedded WebView)

A healthy `jeopardy` module gets a **Play on phone** entry on its Home card and
inside the module screen. It opens a full-screen embedded `WebView` (the app
bottom navigation is hidden while it is up, so the in-page BUZZ button owns the
whole surface) that drives the module's LAN game page. The whole surface respects
`safeDrawingPadding`, so the header clears the status bar, the bottom BUZZ clears
the navigation gesture and the wager field stays above the IME on every phone in
portrait. The TV still owns the game; if the game is not on, the native **Retry**
help explains that it must be launched from the HR54 remote on the receiver's LAN,
and nothing is bypassed.

Before/while the page loads, a bounded health probe checks the configured game
origin's `/health` on the LAN: plain HTTP on port 8135 only, no redirect follow,
no external host and no management bearer. A non-200 answer, an unrelated
captive-portal HTML page, a redirect or a timeout all surface the native **Retry**
help with the explicit LAN URL; the probe cancels when the host changes or the
screen leaves and never runs per frame. Because the page is bundled, an unreadable
asset is an explicit error screen with **Close** and **Retry** (Back closes the
controller, never the app) instead of an endless spinner.

The page is bundled byte-for-byte from
`source/hr54-re/modules/jeopardy/assets/index.html` by the
`generateJeopardyAssets` Gradle task into generated assets
(`build/generated/jeopardyAssets`, gitignored) and registered through the
Android Variant API. The APK asset is always the reviewed module file; no copy
is maintained by hand.

The controller origin is derived from the normalized receiver: the core port is
discarded, HTTPS is forced to HTTP and port 8135 is used
(`http://<host>:8135/`, IPv6 re-bracketed). The page is injected with
`loadDataWithBaseURL` on that exact origin, so relative `fetch('/api/...')` and
`ws://<host>/ws` stay same-origin and DOM-storage player/host identity is
separated per receiver origin. There is no external browser, no intent hand-off
and no HTTPS upgrade.

Only JavaScript and DOM storage are enabled; file/content access and universal
file origins are disabled and no JavaScript bridge is installed. The
`WebViewClient` allows the configured game HTTP origin only and blocks external
hosts, HTTPS upgrades, `intent:`, `javascript:` and `file:`/`content:` URLs; the
single exception is a tightly-validated `rebox-controller://close?role=...`
main-frame navigation (an iframe/subframe close attempt is consumed without
closing). Credential-bearing URLs are never the game origin, the local-leave
listener is only injected on the configured origin (never an arbitrary or
error page), and a denied HTTPS/external main-frame navigation shows the help
instead of a silent blank page. Errors are logged without tokens (only
scheme/host/port). WebAudio stays behind a normal page gesture and SSL
validation is never bypassed.

Back and the toolbar **Close** evaluate the reviewed
`window.ReboxJeopardy.requestExit()`. An injected origin-scoped listener turns the
page's confirmed local `rebox-controller-close` event into the validated close
URI; only that closes the embedded view (returning to the module/Home). If a
host confirmation is already on screen, the back gesture cancels it on the core
and waits for the authoritative ack instead of dropping the gate, and a host
leave disarms any armed/pending quit/restart with `CANCEL` before it fires the
close event (failing closed with clear feedback if the socket drops). A left
page clears its reconnect timer and cannot reopen from a late callback; only the
in-page **Return to game** re-enables it. Quitting
the *game* remains the authenticated receiver command and is never an Android
management action. Pausing the app pauses the WebView; a returning receiver
reconnects with its stored identity; disposing the view destroys it without
clearing WebStorage, and changing receiver closes it cleanly.

## Build and verification

With Android SDK 37, NDK 28.2.13676358, CMake 3.22.1 and Java 17 or newer:

```sh
cd source/android
./gradlew :app:testDebugUnitTest :app:assembleDebug
./gradlew :app:compileDebugAndroidTestKotlin
```

When `XDG_CONFIG_HOME` is set, AGP resolves its default debug keystore under it.
To keep the source app's debug signature (SHA-256 `b178b3ef…`, the stable
`~/.android/debug.keystore`), select the Android user home explicitly:

```sh
cd source/android
ANDROID_USER_HOME="$HOME/.android" ./gradlew :app:testDebugUnitTest :app:assembleDebug :app:compileDebugAndroidTestKotlin
```

The embedded controller adds deterministic host tests for the game URL/origin
policy, WebView security settings, navigation/subresource blocking, the reviewed
close/exit JS contract and the byte-for-byte build asset origin, plus a
`JeopardyWebControllerInstrumentedTest` that drives the real WebView against a
throwaway local HTTP game. That instrumented test is only a claim when executed
on a device/emulator. The bounded `/health` classifier also guards non-primitive
identity fields (a hostile/portal body that puts an object/array where `ok` or
`service` is expected reports Unhealthy instead of throwing).

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
