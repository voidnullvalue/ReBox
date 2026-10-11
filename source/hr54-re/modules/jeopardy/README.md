# Jeopardy module — authoritative engine, LAN service and native TV show

`jeopardy` is one ReBox API 1 `native-app` module. It has three layers:

* **Rules core** — the complete C rules/state machine, the portable `JEPDB1`
  question database, the PC importer and crash-safe persistence, with
  comprehensive host tests. This is the single source of truth for gameplay.
* **LAN service** — a bounded, non-blocking LAN HTTP + WebSocket server that
  owns the very same core, with reconnect auth, PINs, per-viewer redacted
  snapshots and a private native IPC channel. Built as a static-musl
  `bin/module` (the module entrypoint and lifecycle owner).
* **Native TV renderer** — a uClibc/EGL `bin/hr54-jeopardy-native` that is a
  dumb consumer of the redacted public display. It renders the theatrical show
  and sends trusted commands on the private socket. It is never authoritative.

The service process is the single authority. The native renderer only ever reads
`native/public-display.json` (redacted `JPQ_PUBLIC`), the private
`native/metadata.json` (LAN URL, join PIN, host PIN) and writes newline JSON
commands to `native/command.sock`, whose actor is validated by the service.

## Layout

```
src/jeopardy.h          public rules + database + persistence API
src/jeopardy_db.c       JEPDB1 reader, coherent board selection, history
src/jeopardy_rules.c    state machine, actions, scoring, timers
src/jeopardy_persist.c  versioned endian-independent atomic snapshot + history
src/jeopardy_snapshot.c JSON display snapshot with private-data redaction
src/jeopardy_service.*  authoritative service: sessions/tokens/PINs, views,
                        command envelopes, persistence, native IPC, snapshots
src/jeopardy_net.*      bounded non-blocking HTTP + RFC6455 WebSocket server
src/jpq_lifecycle.*     native surface supervisor (lock, reaper, pid identity)
src/main.c              process glue: seeding, shared SDK + LAN/native lifecycle
src/jpq_parse.*         bounded, allocation-free JSON reader (renderer side)
src/jpq_view.*          native view model parsed from the public display
src/jpq_render.*        theatrical scene renderer (pure function of the view)
src/jpq_font.*          packed alpha font atlas blitter
src/jpq_font_data.h     generated Overpass atlas (OFL-1.1, from hr54-ui assets)
src/jpq_qr.*            compact QR encoder (byte mode, ECC L, versions 1..4)
src/jpq_remote.*        TV remote model: keys -> trusted native commands
src/jpq_input.*         native input broker (same protocol as Doom/Games)
src/native_main.c       uClibc/EGL renderer entry point (20 fps, bounded)
assets/index.html       self-contained cobalt/gold phone+host webpage (no CDN)
assets/icon.png         original module icon (cobalt/gold, drawn with Pillow)
assets/make_icon.py     icon generator (host Pillow)
assets/OFL.txt          Overpass font licence (bundled)
tools/import_questions.py   PC importer (Python standard library only)
tools/embed_assets.py       embeds the webpage into the service binary
tools/build_font_atlas.py   bakes the font atlas (host Pillow)
tools/jeopardy-network.sh   module-owned persistent LAN firewall boot hook
tools/install-persistence.py guarded live deploy of the boot hook + launcher
tests/run.sh                acceptance runner (rules + service + native + package)
tests/test_jeopardy.c       host rules/database/persistence tests
tests/test_network.py       raw-WebSocket integration tests (stdlib only)
tests/test_native.c         host native stack: parse/render/remote/QR/lifecycle
tests/test_snapshot.c       native parser vs. the real public display, every
                            phase, signed buzz slots 0/1/5/none, 6 players,
                            a >96-char clue and redacted final answers
tests/test_integration.c    native remote driven through the live service
                            (correct/incorrect/DD/pause/restart/quit/relaunch)
tests/test_sdk_native.py    real SDK-environment lifecycle: /native/start/status/
                            stop with a mock child, quit->relaunch net survival
tests/test_seed.c           shared-SDK default seeding + no-replace test
tests/test_boot_persistence.py  host acceptance for the reboot persistence
                            boot hook + generic launcher (mock iptables, no reboot)
tests/test_webpage.mjs      headless Node VM UI tests for assets/index.html
                            (host quit/restart confirmation, cancel, leave,
                            Android back contract, back-during-armed cancel,
                            leave-with-CANCEL-ack, fail-closed disconnect,
                            no-reconnect-after-leave; node optional)
tests/package_test.py       .rbox archive/mode/limit/architecture checks
tests/mock_engine.c         mock child for the lifecycle test (not shipped)
```

## Build, test and package

```sh
bash source/hr54-re/modules/jeopardy/tests/run.sh
HR54_ZIG=/tmp/hr54-zig/zig HR54_SYSROOT=$PWD/source/hr54-re/extracted/sdb4-rootfs \
  sh source/hr54-re/modules/jeopardy/build-receiver.sh
```

`run.sh` imports the demo/fixture databases, bakes the font atlas, embeds the
webpage, runs the host rules, service and native-stack tests, generates PNG
previews, tests default seeding, drives the live service with raw WebSocket
clients, cross-builds the musl service and uClibc renderer, and checks the
`.rbox`. `build-receiver.sh` produces the installable
`build/jeopardy.rbox` and the two ELF binaries.

### Installation

`build-receiver.sh` writes `build/jeopardy.rbox` (deterministic gzip+USTAR via
`tools/build-module.py`). It is installed through the normal module API/catalog
path (`tools/build-runtime-bundle.py` packages it as a bundled core module).
The package ships `default-data/questions.jdb` (the demo pack); on first start
the shared SDK copies it to `<data>/questions.jdb` at `0600` and **never**
replaces an existing user database.

### Reboot persistence (LAN firewall boot hook)

The module package and the module runtime deliberately cannot open the stock
firewall or edit the persistent launcher, so the reboot-surviving pieces live
**outside** the package and are deployed only by an explicit, guarded helper:

* `tools/jeopardy-network.sh` - a module-owned boot hook installed at
  `/var/hr54-persist/rebox/boot.d/jeopardy-network.sh` (`0700`) with a private,
  root-owned config `boot.d/jeopardy-network.conf` (`0600`). It owns exactly one
  `INPUT` rule for the wired LAN interface (`eth0`), the private LAN
  `192.168.88.0/24` and TCP `8135`, and removes only that rule when the ReBox
  marker, the module manifest and the schema-1 state are absent, disabled,
  uninstalled or malformed. It never widens LAN/WAN reachability and never edits
  any other firewall rule or the stock base policy. It is idempotent even on
  iptables 1.4.7, which has no `-C` and whose busybox has no `grep -x`.
* `source/hr54-re/reboxd/rebox-start.sh` - the generic persistent launcher now
  spawns any regular `boot.d/*.sh` hook in the background, fully fail-isolated,
  after the ReBox marker is confirmed and before the core binds its modules. A
  missing, slow or broken hook can never gate or crash core boot, and the hook
  path is trusted, regular and non-symlinked.
* `tools/install-persistence.py` - the guarded live installer. It pins and
  verifies the reviewed artifacts (with expected markers), reads the receiver's
  current launcher hash and refuses to overwrite an unknown (manually edited)
  launcher, backs up the old launcher/hook/config with a timestamped
  module-version manifest and a generated `rollback.sh`, and writes the
  replacements atomically with fsync and `0700`/`0600` modes. It never runs the
  whole launcher live (only the isolated, idempotent hook) and proves the core
  pid, native pid, current players and LAN health are unchanged.

```sh
python3 source/hr54-re/modules/jeopardy/tools/install-persistence.py \
  --host 192.168.88.103 --interface eth0 --lan 192.168.88.0/24
```

Host acceptance (mock iptables 1.4.7, isolated fake launcher, no reboot):

```sh
python3 source/hr54-re/modules/jeopardy/tests/test_boot_persistence.py
```

The module installer cannot open the firewall automatically: the boot hook is
outside the `.rbox` and is installed only by the helper above.

## Controller controls

The TV remote is the host controller. The native renderer sends validated
`actor:"host"` commands on `native/command.sock`:

| Key | Action |
| --- | --- |
| D-pad UP/DOWN/LEFT/RIGHT | move the board focus (used tiles are skipped) |
| OK | lobby START · board SELECT · clue/daily-double/final judge the buzzing player CORRECT · daily-double wager ADVANCE · reveal/results ADVANCE |
| RED | judge the buzzing/current player INCORRECT |
| INFO | open host settings (resume/restart/quit, score edit, host connection, timers, reveal answer, pass) |
| BACK | pause and open the pause menu (returns to ReBox if in the lobby) |
| STOP | arm the rules quit confirmation, then STOP again to commit (never an immediate cut) |

The clue keys act on the player who actually buzzed (the signed buzzer slot),
never the board selector; the daily double judges the wagerer (`dd_selector`).
Reveal and pass are reachable from the INFO host menu so OK always means
"correct" during a live answer. Quit and restart are the same two-phase
confirmations as the phone host: the first press arms the rules core, the second
commits, and BACK explicitly cancels so a disarmed confirmation can never fire
later.

Phones join at the LAN URL with the **join PIN** shown on the TV; the lobby QR
encodes that URL with `#pin=<joinPin>` so a phone camera scan prefills the PIN.
The **host PIN** is shown only behind the TV's INFO → HOST CONNECTION screen and
the host handset. Board selection is available to the player who currently
controls the board (the selector) as well as the host (TV remote or host phone),
never to spectators. The TV paints each podium in that player's chosen colour
(published in the public display alongside the redacted state, never inside it)
and highlights the actual buzzing player.

## Phone page: exit/cancel and the Android WebView contract

`assets/index.html` is self-contained (no CDN, no external font/script) and is
embedded byte-for-byte into `bin/module`, served from the same receiver origin
(`http://<lan>:8135/`). It stays on plain `http`/`ws` via `fetch('/api/...')` and
`ws://<host>/ws`; it never forces HTTPS and never uses a management or privileged
token — every action rides the ordinary authenticated role (player token or host
token) issued by `/api/join` and `/api/host`.

The page never uses the browser's native `confirm`/`prompt`/`alert` dialogs
(an Android `WebView` does not render them, so the old flow could not exit) and
never double-sends on a timer. All confirmations are in-page modals:

* **Host controls** are pinned in their own bar directly under the top bar,
ahead of the scrolling board and the BUZZ button, so Pause/Resume and Quit are
always reachable.
* **Quit game / Restart** are two-step and receiver-acknowledged. The first press
arms the rules core; the modal then waits for the authoritative `state` to echo
`quit_confirm`/`restart_confirm` before the Commit button enables; the second
press commits. The commit is refused unless the current role is still `host` and
the arming `epoch`/`window` still match, so a stale button can never commit after
a Cancel. **Cancel** sends `JPQ_ACT_CANCEL` and waits for the core to clear the
flags before closing; a commit already on the wire can't be undone, so Cancel is
disabled and the result is shown instead. A dropped connection, a rejected
command or a new `epoch` fails the pending confirmation closed (Commit can never
fire on its own), and a reconnect never auto-quits.
* **Leave controller** is local only and never sends a command on a player's
behalf, so a player keeps their seat/score and an ordinary player can never issue
a host command. It clears the reconnect timer and detaches the socket so a hidden
controller cannot reconnect from a late callback; only **Return to game**
re-enables it. The auth screen's resume path tries both stored tokens through
`/api/join`, so a host can return too. If the controller being left is a host
that still has an armed or pending quit/restart, the leave first sends
`JPQ_ACT_CANCEL` and waits for the receiver to clear the flag before firing the
close event; if that CANCEL can't be sent, or the socket drops while waiting, the
leave fails closed — the phone stays connected with clear feedback and never
pretends the confirmation was cancelled.
* **Android back contract:** the `WebView` calls
`window.ReboxJeopardy.requestExit()` (no `addJavascriptInterface`, no Java
bridge). It uses only the authenticated browser role and returns `true` when it
took over the gesture: hosts get a Quit game / Leave controller / Cancel menu,
players and spectators get Stay / Leave controller. When a host confirmation is
already on screen, the back gesture cancels it on the core and waits for the ack
rather than swapping to the leave menu (which would strand the server flag); a
commit in flight can't be undone and is reported instead. Only a locally
confirmed "Leave controller" dispatches the custom DOM event
`rebox-controller-close` (detail `{role}`); the event carries no credential and
no capability. Quitting the game is always the authenticated WebSocket `QUIT`
command exactly, so the native TV keeps its authority over the exit.

Headless acceptance (Node, skipped when Node is absent) drives the real page
script against a fake DOM and a controllable WebSocket: the host confirmation
gate, explicit cancel, local leave, the Android back contract, the back gesture
during an armed/pending confirmation, leave-with-CANCEL-ack, the fail-closed
disconnect and the no-reconnect-after-leave latch.

```sh
node source/hr54-re/modules/jeopardy/tests/test_webpage.mjs
```

## Importing questions

The importer is Python-standard-library only and writes the portable `JEPDB1`
format:

```sh
# j-archive JSON + jwolle TSV, filtered, into a custom pack
python3 source/hr54-re/modules/jeopardy/tools/import_questions.py \
  --jarchive game.json --tsv clues.tsv \
  --year-min 2000 --year-max 2015 --difficulty-min 0 --difficulty-max 2 \
  --out my-questions.jdb --report

# a JSON pack (custom categories/clues)
python3 source/hr54-re/modules/jeopardy/tools/import_questions.py \
  --pack my-pack.json --out my-questions.jdb --report
```

Place the result as `<data>/questions.jdb` (or point `JPQ_DB` at it) to use it.

## Tested on host vs outstanding on hardware

* **Host-verified:** rules/database/persistence (thousands of checks), the live
  LAN service with raw WebSocket clients (joins, buzzer races, auth,
  replay/staleness, envelopes, timeouts, framing, privacy, reconnect/restart,
  full game), the native stack (bounded JSON parse, redacted snapshot, every
  scene render, remote mappings, QR encode + a self-check decode cross-checked
  against the host `qrcode` reference, lifecycle mock child), default seeding,
  the MIPS32 cross-build and the `.rbox` archive/mode/limit/architecture checks.
* **Outstanding on hardware:** the renderer has not been run on a physical
  HR54. EGL visibility, HDMI mode and long-session stability follow the same
  verified games path but are not claimed here until observed.

## Receiver TV sound effects are currently unsupported

The only audio that reaches the receiver's HDMI output goes through the core
media/decoder pipeline (the working radio/IPTV path: an opaque current item plus
core-owned decoder replacement) or the Doom-specific audio transport, which the
task forbids depending on. There is **no verified interface for an arbitrary
native app to inject sound effects without bypassing core**, so receiver-side
SFX are **currently unsupported**: the required TV tones (buzz, correct,
incorrect, transition, countdown, win) are not implemented on the receiver and
this is not claimed as accepted TV audio behaviour. All game tones are produced
**on the phone** with the WebAudio API, offline and behind a user gesture, with a
mute toggle, which is the only sound path exercised today.

## Limitations

* Receiver TV sound effects are currently unsupported (above); phone audio is
the only working sound path.
* `JPQ_PUBLIC_URL` is autodetected to the receiver's non-loopback LAN IPv4 (via
  `getifaddrs`); pass an explicit URL if autodetection picks the wrong interface.
* Join-PIN rate limiting is process-wide and tokens are plaintext in a `0600`
  sessions file; hashing tokens at rest is a future hardening step.
* The 16-connection bound is fixed at compile time and ample for 6 players + host
  + spectators. In-memory state is single-process.

## Release 0.3.1 — embedded controller exit / Android WebView back contract

`0.3.1` is a patch release over `0.3.0` that ships the reviewed phone-controller
"webexit" work with **no** service, rules or native-renderer behaviour change:

* the self-contained phone page (`assets/index.html`) quits/restarts only through
  the two-step, receiver-acknowledged host confirmation; **Cancel** clears the
  armed flag on the core (`JPQ_ACT_CANCEL`) instead of stranding it; and **Leave
  controller** is local-only — it never sends a command on a player's behalf and
  the `rebox-controller-close` DOM event carries no credential (see "Phone page:
  exit/cancel and the Android WebView contract" above).
* the Android `WebView` back gesture is handled by
  `window.ReboxJeopardy.requestExit()` (no `addJavascriptInterface`, no Java
  bridge): hosts get Quit game / Leave controller / Cancel, players and
  spectators get Stay / Leave controller, and a back press during an armed host
  confirmation cancels it on the core rather than swapping menus.

The service, rules core, native renderer, package layout and the Android
controller APK are unchanged. `tests/run.sh` passes with the page's headless
**96 checks** and the raw-WebSocket LAN **107 checks** (0 failures) and builds
`build/jeopardy.rbox` sha256
`db4685f11278581e3d86a3391924142ddfc95b9442c3e9a6a32c23beaf8a22e9`. The
reviewed `android/ReBox-controller.apk` is byte-identical (sha256
`94ed0b8e9b816ff30bfcca7d83bdc6d170427218e1c23eb73c3b33c815a81b6f`) and was not
rebuilt. `0.3.1` was installed through the normal managed module transaction on
the HR54 `192.168.88.103`; the single persisted player and core pid were
preserved (see `DEPLOYMENT-HR54.md`).

## Release 0.3.2 — persistence recovery: a reloaded lobby resumes live

`0.3.2` is a patch release over `0.3.1` that fixes authoritative persistence
recovery after a module upgrade or reboot; the rules core, native renderer,
package layout and phone page are otherwise unchanged.

* `jpq_load` used to force `paused=1` on every reload. A clean lobby (phase
  `LOBBY`, no attached clue, `timer.kind = NONE`, no timer remainder) is not a
  game in progress, so it now reloads **unpaused**: the preserved players can
  `READY` and the host can `START` immediately. The phone page needs no
  paused-overlay `RESUME` workaround.
* Every in-progress phase still reloads safely paused on its saved timer
  remainder; no active game is auto-started and the native TV surface is never
  auto-launched.
* Interrupted two-step quit/restart confirmations are cleared on reload, so a
  stray next press can never commit a confirmation the operator backed out of.
* Players, scores, clue database, history, `epoch` and per-actor sequences are
  preserved exactly.
* A saved `LOBBY` whose persisted timer is inconsistent fails closed: it stays
  paused rather than auto-resuming a bogus timer.

`tests/run.sh` passes (webpage 96, host rules 3076, native 44, snapshot 656,
network 118, sdk-native 28, boot persistence 46, package 19, and the large
integration suite; 0 failures) and builds `build/jeopardy.rbox` sha256
`23ea113d1c75b82dff8b5dd4205c49de5d2b196475fd50c0da9f021858d6693b`. The
reviewed `android/ReBox-controller.apk` is byte-identical (sha256
`94ed0b8e9b816ff30bfcca7d83bdc6d170427218e1c23eb73c3b33c815a81b6f`) and the
phone page is unchanged (sha256
`39ada7bc5768afba38c733111405059dd7d17d126c22f3d1a5a8e0d314f3d8d3`). `0.3.2`
was installed through the normal managed module transaction on the HR54
`192.168.88.103`; the persisted player and core pid were preserved and the
reloaded lobby came back unpaused (see `DEPLOYMENT-HR54.md`).
