# ReBox modules — API 1

EARLY ALPHA. The module architecture is being introduced in tested phases.
The packaged receiver binaries still use the previous backend until the service
migrations and receiver builds are complete. Host foundation work alone is not
physical acceptance.

A module is a separately packaged executable, identified by a stable ID, with
its own data and process. Core discovers manifests at runtime; client navigation
must use these descriptors. Display names are never IDs. Maximum: 32 modules.

## Filesystem and package format

`/var/hr54-persist/rebox/` contains `modules/<id>/` (installed files),
`module-data/<id>/` (writable state), `module-state/<id>.json` (core state),
`builtin/` (offline packages/catalog), `staging/`, and `log/`.
Directories are 0700; state/secrets 0600; executables 0700. State writes use
`.next`, file fsync, rename, and directory fsync. Module removal preserves data.

A `.rbox` is gzip-compressed POSIX ustar. Its root contains `module.json`.
Only regular files and directories are permitted. All links, device nodes,
FIFOs, absolute paths, dot components, duplicates, and paths longer than 255
bytes are rejected. No archive metadata may set ownership, setuid, or world
permissions. Limits: 64 MiB compressed, 128 MiB unpacked, 4096 entries.
These allow the inspected 27 MiB YouTube runtime with headroom.

```json
{
  "schema": 1,
  "id": "example-media",
  "name": "Example media",
  "version": "1.0.0",
  "description": "An independently packaged provider",
  "moduleApi": 1,
  "minReboxApi": 1,
  "kind": "media",
  "entrypoint": "bin/module",
  "icon": "icon.png",
  "capabilities": {"browse": true, "search": true, "playback": true},
  "ui": {"homeDescription": "Browse a collection", "order": 100}
}
```

IDs match `^[a-z0-9][a-z0-9._-]{0,63}$`. Names: 127 bytes; versions: 63;
descriptions: 255; paths: 255; manifest: 16 KiB. Unknown additive fields are
ignored. Required schema/API major versions must equal 1; incompatible installed
modules remain visible, disabled with an error. Native apps use `kind` =
`native-app`, `capabilities.nativeApp` = true, and `presentation.releaseSurface`
and `presentation.releaseInput` as needed.

`GET /api/modules` returns `ok`, `moduleApi`, and `modules`. Descriptors include
ID/name/version/description/kind, installed/enabled/core/compatible/healthy,
capabilities, presentation, Home visibility/order, restart count, and error.
Executable paths and secrets are never included. Core readiness is independent
of optional module availability.

## Built-in trust

The distribution-generated `builtin/catalog.json` contains schema 1 and a
`modules` array of ID, name, package filename, actual SHA-256, and defaultEnabled.
Only this trusted catalog establishes core identity. A manifest's `core` field
is ignored. Missing core modules remain visible for offline reinstall; missing
third-party modules do not. Packaging may enumerate bundled providers; runtime
code must not enumerate them.

## RPC contract

Each enabled module runs out of process and listens on its core-assigned Unix
socket. Core passes `REBOX_MODULE_ID`, `REBOX_MODULE_SOCKET`, `REBOX_MODULE_DATA`,
and `REBOX_MODULE_API=1`. No module socket is exposed on the LAN. HTTP/1.0 or
HTTP/1.1 requests use JSON, explicit Content-Length, and Connection: close;
chunked JSON responses are not part of this ABI. Status must return
`{"ok":true,"moduleApi":1}` without contacting an optional upstream server.

The operations are GET `/status`, `/browse`, `/search`, `/settings`,
POST `/settings`, `/actions/<opaque-action>`, `/play`, `/playback/stop`,
`/playback/pause`, `/playback/resume`, `/playback/seek`, and native-app
POST `/native/start`, GET `/native/status`, POST `/native/stop`.
Only declared operations may be invoked. Public equivalents use
`/api/modules/<id>/...`; transport uses `/api/playback/...`.

Browse accepts optional `parent` (opaque ID), `offset` and `limit`; search takes
`q`, `offset`, `limit`. Responses are:

```json
{"items":[{"id":"folder-a","title":"Folder A","kind":"folder","playable":false},
{"id":"item-1","title":"Item 1","kind":"item","playable":true,
"subtitle":"Example","description":"Description","artwork":"item-1",
"year":2026,"duration":120,"resume":0}],"total":2,"offset":0,"hasMore":false}
```

Kinds are folder/item/action. Root hierarchies belong to modules. IDs must be
percent-encoded as query/path components; core never interprets provider IDs.
Icons are package PNGs; artwork is proxied through `/api/modules/<id>/art/<id>`
from `/art/<id>`. Clients use bounded decoding and a generic fallback.

Settings use a `fields` array with key/label/type/value and optional choices.
Initial types: bool, string, integer, choice. Actions use ID/label; action
responses can expose a code, explanatory message, pending state, and a poll
operation. Authentication mechanisms belong to modules, including Quick Connect.

Media `/play` takes `itemId` and optional `startSeconds`. It prepares and returns
an inert playback plan: type `stream`, title/live/duration, stream kind
`moduleProxy` with opaque token (or validated `http` URL), transport flags,
and an opaque `session`. Core arbitrates the one decoder and invokes its fixed
playURL helper. A module must never invoke the receiver player itself. Stream
bytes come from `/stream/<token>`, incrementally, and only a registered current
session may be proxied. Stop cancels preparation and releases upstream resources.
Native apps own executable/assets/lifecycle; core owns surface/input handoff.

## Migration and implementation checkpoints

Jellyfin's initial module will retain `/var/hr54-persist/jellyfin` for credentials
and caches. IPTV's playlist is currently `jellyfin/iptv/eng.m3u`, with checkpoint
`jellyfin/state/iptv-state.json`. YouTube runtime is `jellyfin/youtube`, checkpoint
`jellyfin/state/youtube-state.json`. Preserve these until validated copy/verify/
atomic activation migrations exist. Doom assets currently live in
`jellyfin/doom`. Frigate currently has compiled configuration. No migration has
yet been activated by the foundation build.

Build/check the foundation: `make -C source/hr54-re/reboxd test`.
The production launch script and accepted payload must be changed only after
module extraction, compatibility, frontend, and packaging checks pass.

## Management authorization and install transaction

Core creates a random 256-bit `module-state/management.secret` (0600). Trusted
local UI can read this file to authorize management; ordinary LAN responses
never return it. A management bearer opens `/api/management/pair/open`, which
returns an eight-character code valid for 120 seconds and at most five failed
attempts. `/api/management/pair` exchanges it once for a random persistent bearer.
The current implementation supports one paired controller: pairing replaces
and revokes its predecessor. POST `/api/management/revoke` revokes it. Tokens
are stored 0600 and never logged. Android must store its bearer in private,
backup-excluded app storage, scoped to its receiver address.

POST `/api/modules/install` takes `url` and optional lowercase `sha256`.
POST module `/enable`, `/disable`, `/reinstall` and DELETE module require the
bearer. Read-only discovery remains LAN-visible. Active playback currently
makes lifecycle mutations return 409; stop it before retrying.

The receiver downloads via a fixed curl helper (TLS verification, HTTP/HTTPS
only, capped redirects/time/bytes; HTTPS redirects cannot downgrade to HTTP).
A controlled ustar reader extracts only validated regular files/directories
into a random staging directory. It rejects links, devices, traversal, duplicates,
malformed checksums and oversized data. It never invokes tar extraction.
A supplied digest must match. Replacing a catalog ID additionally requires its
catalog digest, so another package cannot impersonate the bundled executable.

Before promotion, a durable journal preserves prior state. Live files move to
an adjacent backup, staged files are renamed into place, and the process must
pass its `/status` handshake before state is committed. Failure restores files
and state and restarts the old process. Boot rolls back an interrupted journal
before serving discovery. Offline reinstall verifies the retained catalog hash.
Third-party uninstall removes registry state but keeps writable module data.

**Third-party modules execute receiver-side code and must only be installed
from trusted sources.** They currently inherit ReBox's receiver privileges;
process separation isolates crashes, not malicious code. Management API is
privileged. Pairing does not secure the existing root shell/TFTP services or
provide encryption against a hostile LAN. Use a trusted isolated network.

## Jellyfin migration checkpoint

The Jellyfin module now implements the generic root (All titles, Continue
Watching, libraries), folder/search results, artwork, quality fields, and auth
actions. Its process owns Quick Connect, the persisted device/token state,
PlaybackInfo, relay gating, seek, long-pause restart, and decoder-drain timing.
A seek or long-pause resume returns a replacement stream plan; it never starts
the vendor player itself. Core validates replacement plans through the same
path as initial playback, cancels the prior proxy epoch, and commits a new
generation only after the decoder accepts the new stream.

The module deliberately reuses `/var/hr54-persist/jellyfin` when that existing,
receiver-owned directory is present. There is no move, deletion, or rewrite of
its existing token/device ID as a migration step. Host fixtures use the supplied
module-data directory. Legacy field/route adapters live in `reboxd/compat.c`;
legacy response shapes are serialized by the same Jellyfin implementation.
During the incremental migration only, the old daemon build includes that
module-owned source; it no longer maintains another copy of those functions.
The production payload still starts the old daemon until the other migrations
and packaging changes are complete.

The threaded module SDK serves up to eight requests so a stream does not block
status or Stop. Core likewise reserves bounded request workers, publishes a
read-only registry snapshot, and allows cancellation while preparation is in
progress. Proxy buffers are 32 KiB and responses are capped; streams are never
buffered whole. An internal session epoch prevents an old disconnected stream
from clearing a newer session. Module `/status` may report
`"playback":{"session":"opaque-session","playing":false}` after decoder drain
so core can retire the matching session. Transport RPC bodies carry the core's
current `session`; modules must ignore stale-session stop requests.

Validation for this checkpoint:

```sh
make -C source/hr54-re/reboxd test
bash source/hr54-re/jellyfin/remote/test/run_host_tests.sh
ZIG_GLOBAL_CACHE_DIR=/tmp/rebox-zig-cache \
ZIG_LOCAL_CACHE_DIR=/tmp/rebox-zig-local \
sh source/hr54-re/reboxd/build-receiver.sh
```

The host build uses a mocked vendor decoder only; the real Unix socket relay is
exercised against the local Jellyfin fixture. The MIPS core and Jellyfin module
cross-compile, but the receiver download helper currently cannot link in this
checkout because the extracted vendor `libcurl.so.4.4.0` is absent. No payload
binary or distribution checksum has been replaced with an unbuilt artifact.

## Native client checkpoint

Native Home, browsing, search, artwork, and settings now use runtime module
IDs. Search-first modules open the keyboard based on capabilities. A bounded
neighbor viewport handles zero through 32 Home entries; disabled modules stay
in the manager. Settings contains Modules and System/About. Module detail
supports enable, disable, uninstall while keeping data, offline reinstall,
module-defined fields/actions, and management pairing. The separate URL entry
buffer holds 1024 bytes; search keeps its 64-byte limit.

The native client is a trusted local management client. It reads only the
core's private `module-state/management.secret`, then sends the bearer to the
loopback core listener. It does not read service configurations. A host-only
fixture override exists for integration tests and is excluded from receiver
builds. Android must use pairing instead of this file.

There is one active media page and a 12-entry history of opaque cursors and
selections. Measured host `App` storage fell from 3,555,432 bytes to about
191,520 bytes. The eight-entry image cache is shared by module icons and media
artwork, with one decoder worker. Icons decode to 128×128; existing compressed
size, dimension, and decoder-allocation caps remain. Home no longer links a
five-service atlas. Missing/invalid icons use a shell-owned generic symbol.

`/api/system/status` identifies active native presentation through
`nativeModule` (an empty string when idle). `mediaBusy` remains the conservative
activation/idle gate: missing busy information is treated as busy. Playback
state includes an opaque core `instance` so clients can discard old generation
counters after a daemon restart.

```sh
HR54_TEST_POLICY="$PWD/receiver/payload/hr54-persist/native-menu/dtv-menu-policy.car" \
  make -C source/hr54-re/hr54-ui test test-modules
make -C source/hr54-re/hr54-ui test-module-integration
python3 source/hr54-re/hr54-ui/tools/previews.py
```

The integration tests drive the real host frontend, real core, and real Unix
socket module processes. One types a locally hosted `.rbox` URL through the
production keyboard, installs an ID generated after compilation, navigates
folders, searches, plays, disables, re-enables, and uninstalls without restarting
or rebuilding the frontend. Another exercises Jellyfin browsing, quality
settings, disconnect/connect and automatic auth-action polling through the
same generic UI. The decoder is mocked; this is not physical HR54 acceptance.

## IPTV migration checkpoint

`modules/iptv` owns the bounded M3U cache/parser, stable channel IDs, group
hierarchy, tvg metadata, logo fetches, TLS helper, HLS variant selection and
MPEG-TS relay. Generic root returns All channels and opaque group folders;
search returns ordinary playable items. It prepares an inert moduleProxy plan
with Stop only; core owns decoder arbitration. Stop cancels upstream preparation
and relay without taking the operation mutex. Stale session stops are ignored.

On the receiver the module deliberately reuses the existing receiver-owned
`/var/hr54-persist/jellyfin` directory for `iptv/eng.m3u`, certificate bundle and
`state/iptv-state.json`. No playlist or state is moved or deleted. Fresh host
fixtures use module-data. The packaged helper is `bin/fetch`; it runs through
fixed argv, never shell interpolation. SNTP bootstrap runs separately from the
module listener so optional network availability cannot block health/readiness.

Legacy `/api/iptv/{groups,channels,status,state,play,stop}` endpoints in reboxd
are adapters. The play adapter translates channelId and preserves channelId /
channelName response fields; stop remains cancellable while preparation runs.
The old distribution daemon temporarily includes the same module-owned parser
and relay with its old decoder wrapper, as with the Jellyfin checkpoint. It is
not the final production startup path.

Validation:

```sh
make -C source/hr54-re/reboxd test
make -C source/hr54-re/hr54-ui test-module-integration
ZIG_GLOBAL_CACHE_DIR=/tmp/rebox-zig-cache \
ZIG_LOCAL_CACHE_DIR=/tmp/rebox-zig-local \
sh source/hr54-re/modules/iptv/build-receiver.sh
```

The module executable cross-compiles for MIPS. The last command cannot complete
the receiver TLS helper in this checkout: extracted `libcurl.so.4.4.0` is absent.
Host tests use the real curl helper against a local upstream, validate TS/HLS
bytes, redirects/headers, sequence refresh, cancellation, EOF, rejected formats,
legacy state and runtime discovery. A real native UI test opens All channels
and a group folder and plays their opaque items through the generic frontend.
These tests do not validate Broadcom playback, real channel compatibility or
receiver boot. Production artifacts/checksums have not been replaced.

The historical 2,096-channel parser comparison could not run: its source
playlist is absent, and the distribution playlist is a different fixture.
The new local playlist tests cover metadata, stable IDs, groups and reload.

## YouTube migration checkpoint

`modules/youtube` owns the resolver facade, idle updater, URL validation, worker
lifecycle, remux relay and saved search state. Its manifest declares browse=false
and search=true; no client ID check opens search. Generic pagination translates
opaque results into item/title/subtitle/duration fields. Playback preparation
returns an inert moduleProxy stream plan with Stop only. The module retains the
startup/drain hold and one fresh signed-URL resolution before any stream bytes.
Core alone owns decoder commands and session arbitration.

The receiver initially retains `jellyfin/youtube` runtime/cookies and
`state/youtube-state.json` without moving/deleting files. Resolver/updater/relay
sources have moved into the module; transitional runtime builds reference them.
The Python tools take an explicit runtime directory from the module process;
the updater checks generic core ready/mediaBusy state rather than checking a
fixed list of services. Automatic updates are a persisted module setting,
protected through the generic settings API. Missing resolver/upstream failure
never prevents module/core health readiness.

Legacy `/api/youtube/{search,status,state,play,stop}` routes delegate to module
RPC and core playback. The play adapter translates videoId. Legacy Stop can
cancel search or playback preparation without waiting for the operation lock.
The old daemon temporarily includes this module-owned implementation for the
unconverted distribution path. Full runtime package seeding remains packaging
work; this checkpoint has not replaced receiver payload assets.

Validation: core tests now include resolver subprocesses, normalized search,
legacy state, invalid IDs, stream bytes, preparation/search cancellation,
settings authorization/persistence and missing-runtime readiness. The real
native UI opens search from the manifest, types through its keyboard, browses
results and plays/stops. The legacy worker integration and exact-PID/FIFO fork
failure tests pass. `modules/youtube/build-receiver.sh` builds the MIPS module
and relay; `jellyfin/youtube/build-native.sh` builds runtime helpers and Python
bytecode. These are fixture/remux/build checks, not actual YouTube decoding or
physical HR54 acceptance.
