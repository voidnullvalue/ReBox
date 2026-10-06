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
