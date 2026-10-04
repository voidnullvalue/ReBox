# Native shell: supported stock-context suppression

Scope: HR54-700 extracted firmware and read-only receiver checks on 2026-10-04.
This directory supplies an offline-tested configuration helper, not proof of a
successful live suppression. No receiver configuration or service was changed
by this investigation.

## Narrow supported control

The exact live file is
`/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str`.
Its bytes were `true` (no quotes/newline); the sibling `Context-Main.str` was
exactly `com.directv.druid.DruidMain`.

In `opt/dtv/dtv.car`, `com.ucentric.core.Core` at `0x10029f6` loads contexts
in `Context/Order` and tests `Context.isAutoStart` before `startContext`
(`startup` bytecodes 303/312). `createContext` reads `Context-Main` and
`Auto-Start`. `Context` at `0x1001c4c` loads its main through `Class.forName`
(`startContext` bytecode 18), requires a `ContextMain` factory/type, and starts
the Java context. There is no observed executable/shell `Context-Type` branch.
Putting a native binary/script path into `Context-Main` or adding it to
`Order.str` is not a supported native launcher.

Setting only DRUID auto-start false is consequently a supported way to avoid
starting the stock Java UI producer on the next normal middleware start. It
does not stop an already-running context. It does not guarantee every other
service is noninteractive or that all cold-boot graphics disappear. Retain
Siege, keydispatcher, DTVWM, avoutputmanager, discovery, media/session services,
CA manager and UIUM/Uconnect. Do not globally disable compositor planes or
destroy vendor shared drawlists: native EGL uses the retained DTVWM renderer.

## Local video dependencies

Recovered CAR instructions support a test of DRUID suppression without
replacing the local media stack:

- `Core.startup` calls `DiscoveryManager.serviceInitDone` after configured
  context startup. `StartupListener` (`0x101a8ba`) selects the receiver startup
  implementation after discovery; `StartupBase` (`0x1019f6e`) creates the local
  session at startup bytecode 94.
- `UiSessionManager` (`0xb6967c`) creates/registers the local session;
  `LocalSession` (`0x106fe23`, constructor `0x107080e`) creates its player
  manager, players and buffer manager. This is outside DRUID.
- `UISessionMgr` (`0x3915fe`) `startLocalSession` reads the already-existing
  `Session.getLocalSession` (bytecode 38) and wraps it in a `LocalUISession`.
  `UISessionMgr.initialize` is not what allocates the middleware local player.
- The decoded external caller of `DvrCoreSingleton.startDvrCore` is
  `CAManagerApplicationEvent` (`0x108b7b1`), normal
  `handleCaModuleConfigEvent` bytecode 103. Preserve its normal dispatch and
  existing CA behavior unchanged; no event fabrication or access/security
  settings are needed or authorized for this UI repair.
- `DvrCoreSingleton` (`0xb7ac8d`) registers the DVRCore startup callback;
  `handleDvrCoreStartup` configures native resources and calls
  `Session.getLocalSession().initializeMediaPlayers` (bytecodes 104/107), then
  marks initialization complete. LocalSession initializes its main player
  there. `DruidIsReady` separately enables RVU services: remote Genie/RVU
  behavior is a dependency caveat, not validated by local URL playback.
- Live `Ucentric.Pvruconnect/Auto-Start.str` was already `false` while URL
  playback worked. `Ucentric.UIUconnectMgr` main is
  `com.ucentric.uiUconnectMgr.UIConnectContextMain` (`0x1228d0c`); it retains the
  listener/server named UIUM. DirectTest is handled by that retained transport,
  not evidence that the disabled legacy context must be enabled.
- `DirectTest.playURL`, disassembled in
  `docs/native-evidence/DirectTest.javap`, selects the existing local media
  player when `ui` is omitted, then `watchRemoteStream`. Suppress no media,
  Uconnect or decoder service on the basis of its name.

The CAR reader's temporary analysis adaptation recognized self-describing
generic signatures and UISessionMgr bootstrap constant-pool entries; recovered
class-end bounds were checked. Access flags in host `javap` containers are
normalized for disassembly, not executable replacements. This static evidence
does not substitute for the live validation below.

Do not use `STB_IS_HEADLESS`/HeadlessConversion as an alternative. `playURL`
refuses the ordinary local-player path when headless, and
`HeadlessConversionMgr` (`0x268899`) changes local session/headless state,
default-channel/video cleanup, mute/OSD and Genie-client topology.

## Persistence and early boot limits

`run_ucentric.sh` explicitly sets `DIRECTV_REG=file:/var/mw_registry/Registry`.
The ordinary middleware start script does not restore that registry, so the
edited file should be consumed on a subsequent `dt run`; check it before/after
the actual reload rather than assuming an untested live result.

The cold-boot reset **is proven in the extracted scripts**:

1. `etc/init.d/rcS:727` runs `import_files.sh`.
2. `import_files.sh:167` invokes `/root/check_registry.sh`.
3. `check_registry.sh:19` restores `registry_static.tar` into `/var` on every
   normal invocation with an existing registry.
4. `/root/registry_static.tar` contains DRUID `Auto-Start.str`, `Context-Main.str`
   and CORE `Context/Order.str`, including DRUID auto-start `true`.

A plain registry edit is therefore NOT a permanent cold-boot policy. The
existing persistent native bootstrap must deliberately reapply desired DRUID
policy before its one normal policy reload, after idle/readiness validation,
and report failures. Helper `--status` reports configuration only, not runtime.
Firmware/reset/reinitialization can restore defaults independently.

`rcS:774` enumerates `/etc/init.d/S??*`; `S90ucentric` starts `dt run` in the
background. `run_ucentric.sh` starts appstarter, avoutputmanager, DTVWM,
keydispatcher and then runsiege. `/var` is writable but `/` is read-only
squashfs. No supported pre-Siege mutable shell-hook was found; the existing
late native launcher is not an early init hook. The `/var/opt` tar route in rcS
is development-image behavior, not a demonstrated production override. CORE
context ordering cannot directly solve this with a native executable.

Until an authorized supported earlier launcher is established, honest boot UX
can improve native readiness/errors/animation and remove its stuck frame, but
cannot promise immediate control of the firmware's earliest splash period.
Disable testing must independently start the native backend/supervisor: do not
depend on a stock Druid menu action or Guide key to launch the replacement.

## Helper and isolated tests

Use `sh druid-context-policy.sh --status`, `--disable`, or
`--restore /var/hr54-persist/native-menu/backup/druid-context.XXXXXX`.
Only the validated exact DRUID `Auto-Start.str` is replaced. Each actual change
backs up its preimage, Context-Main and target marker in a unique directory;
restore itself saves a reverse backup. Unchanged operations create no backup.
Permissions are retained; no sibling context/headless/Order changes occur.
An interrupted operation can leave an explicit stale lock; inspect it and any
backup rather than blindly removing another operation's lock.

Mutations require root and successful `hr54-ui --check-api --idle` twice. That
checker consumes typed `/api/system/status` with native readiness and
`mediaBusy`/`doomRunning`; backend mediaBusy includes Jellyfin, IPTV, YouTube
and native game activity. Missing/malformed/unavailable/busy is refusal. This
does not atomically serialize remote API launches: keep the Android/controller
idle throughout the maintenance/reload window. It is not a decoder-idle proof
for stock TV playback; that must be accounted for separately before reload.

Run `python3 test_druid_context_policy.py` and `sh -n druid-context-policy.sh`.
Tests cover read-only status, wrong class/value/encoding, missing checker,
media/game/unavailable/malformed checker refusal, non-root, symlink refusal,
foreign backup refusal, unique/idempotent backups, reverse restore, metadata
and other-context preservation. They exercise rewritten isolated paths only;
they do not contact the receiver or claim a deployed change.

## Bounded live validation and rollback

Before any reload: have root recovery available independently of Druid, record
existing registry and service health, deploy known-good native backend/UI/input
policy/supervisor, stop API media/game with the authorized controls, and prevent
new controller launch requests during maintenance. Test native input ownership
and retained EGL readiness. Take physical HDMI/surface evidence, not only HTTP
success. Use helper disable, record its backup, and combine with the already
planned single normal middleware/policy reload; the helper never invokes one.

Existing supported read-only diagnostic queries (UIUM transport) are:

```sh
/opt/middleware_core/system/tv/uconntest '<com.ucentric.pvruconnect.DirectTest command="sessions"/>'
/opt/middleware_core/system/tv/uconntest '<com.ucentric.pvruconnect.DirectTest command="getSpeed"/>'
/opt/middleware_core/system/tv/uconntest '<com.ucentric.pvruconnect.DirectTest command="getPosition"/>'
```

`sessions` is a read-only list of existing Session/local-player references
(DirectTest bytecode 4104 onward); no guessed allocation verb is required.
Position may legitimately be N/A for remote-stream content. Speed is the
controller's state, not a decoder acknowledgement. Query before/after reload
and confirm no absent-session/exception result, UIUM health and stable
CA/DVRCore/DTVWM processes. Then launch a known playable URL through the normal
API/Android path (without a Druid `ui` argument), observe actual moving video
and audio, menu transparency/dismissal, Guide return, all remote controls and
repeated play/stop/game handoffs. Check screen leaks and native launcher
survival. RVU clients require separate validation if in scope.

If session/player/transport/video or native recovery fails, stop any newly
started API/game activity, helper-restore the recorded backup and perform a
separately authorized normal reload. If the backend checker itself is broken,
the helper correctly refuses; the preexisting root recovery path must verify
idle and restore the exact known backup as a manual recovery action. Never
expand recovery to kill the shared media/compositor or change headless/CA.
Cold boot and post-boot reapplication require a separate observed test before
describing this repair as persistent or stock-interface coverage as complete.
