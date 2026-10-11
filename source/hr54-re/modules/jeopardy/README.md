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
