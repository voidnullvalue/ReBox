# Jeopardy module - guarded module-only install to HR54 192.168.88.103

Status: **installed, healthy, native lobby left running and ready**. One module
(`jeopardy`) was added through the core management transaction. No core,
firmware, vendor, catalog or other-module content was changed.

## Package identity

- source: `source/hr54-re/modules/jeopardy/build/jeopardy.rbox`
- sha256: `cbfe4c3f206f8444aa034a44197da30f63eff1f1e68c0fa42919a56156e4ed29`
- manifest: `id=jeopardy`, `version=0.3.0`, `kind=native-app`, `moduleApi=1`,
  `nativeApp=true`

## Install command (acceptance)

```
python3 source/hr54-re/modules/jeopardy/tools/deploy-receiver.py \
  --host 192.168.88.103 \
  --package source/hr54-re/modules/jeopardy/build/jeopardy.rbox \
  --sha256 cbfe4c3f206f8444aa034a44197da30f63eff1f1e68c0fa42919a56156e4ed29
```

Observed:

```
receiver idle: True (mediaBusy=False nativeOwner='' nativeRunning=[])
serving jeopardy.rbox at http://192.168.88.25:43531/jeopardy-cbfe4c3f206f.rbox
install HTTP 200: {"ok": true, "module": {... "id":"jeopardy","version":"0.3.0",
  "installed":true,"enabled":true,"healthy":true,"core":false,"kind":"native-app" ...}}
installed: jeopardy version=0.3.0 installed=True enabled=True healthy=True core=False
```

The deploy tool:

1. verifies the local package SHA-256 before touching the receiver;
2. verifies the receiver is idle (no active media playback, no native owner, no
   native-app child running) and aborts without disturbing a busy session;
3. serves the exact package from the development host LAN address and shuts the
   server down afterwards;
4. installs through `POST /api/modules/install` using the receiver-local
   `module-state/management.secret`, read on the receiver by a script run over
   the existing `tools/recv/hr54-shell.sh` root transport. The bearer is never
   printed, logged, or placed on a command line;
5. re-reads `/api/modules` to confirm a healthy install.

The receiver has no `curl`/`wget` POST client; the on-box HTTP client is the
module runtime's CPython (`urllib`), which keeps the secret local to the box.

## Firewall note (environment, not a module fault)

The module binds `0.0.0.0:8135` correctly, but the stock HR54 firewall only
admitted LAN TCP `8130` and `5777`; all other TCP was dropped. A single,
reversible rule mirroring the existing `8130` rule was added so LAN phones (and
the smoke test) can reach the module's documented LAN service and keep the
phone page responsive:

```
iptables -I INPUT 1 -i eth0 -s 192.168.88.0/24 -p tcp --dport 8135 -j ACCEPT
```

Remove it with `iptables -D INPUT -i eth0 -s 192.168.88.0/24 -p tcp --dport 8135 -j ACCEPT`.
No firmware or persisted config was modified.

## Reboot persistence (generic launcher + module boot hook)

The LAN rule above was made reboot-surviving **without** touching the module
package, core, firmware or vendor content, and **without** restarting the core
or the native surface (pids `5696` / `31562` were unchanged).

- The generic launcher `source/hr54-re/reboxd/rebox-start.sh` (sha256
  `76e282f95c5fa943146f66bd01a8d9b948ae37f7a67fde6797f6b3bb27cf2442`) now spawns
  any regular, non-symlinked `boot.d/*.sh` hook in the background, fully
  fail-isolated, after `REBOX_ENABLED` is confirmed and before the core binds its
  modules. A broken or slow hook cannot gate or crash core boot.
- Module-owned hook `/var/hr54-persist/rebox/boot.d/jeopardy-network.sh`
  (`0700`, sha256 `524b597df14818ee99bd1badaeef890faf7a5f56b22079e0f304c764067aa801`)
  with private config `boot.d/jeopardy-network.conf` (`0600`), scope `eth0` /
  `192.168.88.0/24` / TCP `8135`. It owns exactly one `INPUT` rule and removes
  only that rule when the module is absent, disabled, uninstalled or its state
  is malformed; it never widens LAN/WAN or edits the base policy. It is
  idempotent on iptables 1.4.7 (no `-C`; busybox has no `grep -x`).

The hook lives **outside** the `.rbox` and is never installed automatically by the
module runtime/package; only the guarded helper below deploys it.

```
python3 source/hr54-re/modules/jeopardy/tools/install-persistence.py \
  --host 192.168.88.103 --interface eth0 --lan 192.168.88.0/24
```

Observed (a deploy run, then an idempotent re-run):

```
pre-probe: launcher_sha=e3de6ec6f6cc hook_present=False rule_count=1 core_pid=5696 native_pid=31562 players={'nextPlayerId': 8, 'players': [7]}
apply HTTP 200: {'actions': ['launcher deployed (backup /var/hr54-persist/backup/jeopardy-persistence-20261011-073949)', 'boot hook installed', 'boot config installed', 'rollback routine written', 'hook run rc=0'], ...}
apply HTTP 200: {'actions': ['launcher already current', 'boot hook already current', 'boot config already current', 'hook run rc=0'], ... 'rule_count': 1}
PASS persistent launcher hash is the reviewed content
PASS boot hook hash is the reviewed content
PASS boot hook mode 700 / boot config mode 600
PASS exactly one scoped firewall rule
PASS core pid unchanged / native pid unchanged
PASS current players unchanged / LAN health reachable
PERSISTENCE INSTALL: OK
```

Backups and rollback: `backup/jeopardy-persistence-<timestamp>/` holds the
previous launcher/hook/config (when changed), `manifest.json` (timestamp, module
version, old/new hashes, scope) and a generated `rollback.sh` (`0700`) that
atomically restores them and removes the owned rule. An unknown (manually
edited) remote launcher hash makes the helper STOP without overwriting.

Host acceptance runs with a mock iptables 1.4.7 and an isolated fake launcher,
with no live receiver and no reboot (fresh boot is simulated in a private mount
namespace):

```
python3 source/hr54-re/modules/jeopardy/tests/test_boot_persistence.py
```

No physical reboot was performed; persistence is validated by the hook and
generic-launcher tests and the deterministic fresh-boot simulation.

## Verified on hardware

| Item | Observed |
| --- | --- |
| `/api/modules/jeopardy` | installed, enabled, healthy, compatible, `core=false`, `nativeApp=true`, `restartCount=0` |
| service binary | `bin/module` (static musl), running, 2 threads |
| native binary | `bin/hr54-jeopardy-native` (uClibc/EGL), running as PID 24968 |
| native status route | `GET /api/modules/jeopardy/native/status` -> `{"running":true,"pid":24968}` |
| native start route | `POST /api/modules/jeopardy/native/start` -> `{"running":true,"pid":<new>}` |
| native owner | `module-state/.native-owner.json` -> `{"module":"jeopardy"}` while active, `{"module":""}` after stop |
| PID/lock | `module-data/jeopardy/native/jeopardy.pid` holds the renderer PID; `native.lock` flock held for the surface lifetime and released on stop |
| EGL log mode | `native-egl:` tracing: EGL 1.1, display `0x3000`, `eglInitialize=1`, 1 config, context + window surface + `makeCurrent=1`, `output=720x480 texture=720x480 depth=2000`, all GL ops `GL=0x0`, `eglSwapBuffers=1` |
| memory (hot) | module service `VmRSS=380 kB / VmSize=8688 kB`; native `VmRSS=4752 kB / VmSize=12628 kB` |
| CPU (hot) | module ~0.7%, native ~0.0% at idle (vsync-bound) |
| service persistence | `questions.jdb` seeded `0600` from `default-data`; `jeopardy-v1.bin` snapshot + `jeopardy-sessions.json` written; demo data only |
| public clock | private public-display carries a positive monotonic `now_ms` (default date/clock)

## LAN smoke test (real receiver, 192.168.88.103:8135)

Two raw RFC6455 WebSocket players plus a dedicated host, over the LAN:

```
PASS  LAN /health ok
PASS  LAN phone page served
PASS  phone page has no external URLs
PASS  two raw-WebSocket players joined
PASS  player START rejected (host only)
PASS  both players ready
PASS  host START reached BOARD
PASS  replayed sequence rejected
PASS  judge -> REVEAL
PASS  correct answer scored
PASS  daily double present
PASS  daily-double wager private from other player
PASS  host sees private DD answer
PASS  DD answer hidden from other player
PASS  reconnect restored player identity
PASS  reconnect preserved score
PASS  RESET_LOBBY returned to lobby
PASS  RESET_LOBBY cleared scores
PASS  test seats removed
PASS  final state: empty lobby ready
SMOKE: 20 checks, 0 failures
```

Covered: unauthorized host actions, replayed/nonce-guarded command envelopes,
ready/select/buzz/judge/reveal, a private daily-double wager and answer (hidden
from the other player and spectators), reconnect with score preservation, and a
supported host reset. PINs were read from the receiver's private metadata path
in memory only and are not recorded here.

## Native stop / relaunch (ownership round-trip)

- `POST /api/modules/jeopardy/native/stop` -> `{"running":false}`;
  `.native-owner.json` reverted to `{"module":""}`, `jeopardy.pid` removed, no
  renderer process left.
- `POST /api/modules/jeopardy/native/start` -> `{"running":true,"pid":24968}`;
  `.native-owner.json` -> `{"module":"jeopardy"}` and a fresh EGL init session
  appeared in `log/jeopardy.log`.

## Final state left on the receiver

- Jeopardy module installed, enabled, healthy; **module service not stopped**.
- Native renderer relaunched and running (real `hr54-jeopardy-native`, not mock),
  core still records `nativeModule=jeopardy`, `mediaBusy=true`.
- Game reset to a clean LOBBY: phase 0, no players, no scores.
- LAN phone page `http://192.168.88.103:8135/` returns HTTP 200 (28585 bytes).
- Temporary development-host download server stopped.
- Other modules, credentials and data untouched.

## Observed vs not observable

- **Observed:** EGL surface creation/make-current/swap on the receiver, renderer
  process, PID/lock, ownership handoff, LAN HTTP/WebSocket gameplay, private
  daily-double redaction, reconnects, persistence files, monotonic clock.
- **Not observable from this session:** the physical TV picture and HDMI audio.
  The public display carries a monotonic `now_ms` for on-screen countdowns.
- **Known limitation:** receiver-side TV sound effects are unsupported by the
  module (no verified core API to inject arbitrary native audio); all game tones
  are produced on the phone via WebAudio. This matches the module README.

## Hardware limits (unchanged, from the module docs)

- uClibc/EGL renderer at a fixed 720x480 surface, 20 fps, depth 2000; long-session
  HDMI stability follows the verified Games path but is not separately claimed.
- LAN service is bounded to 16 WebSocket connections and 6 players; join/host
  PINs are the only auth and are rate-limited process-wide.
