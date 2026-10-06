# HR54 media HTTP API

Base URL: `http://<receiver>:8130`. Production implementation: `hr54_jf.c`.
JSON request bodies use `Content-Type: application/json`. Responses retain
existing fields and add `ok:true`; errors are `{"ok":false,"error":"reason"}`.
No Android, speech, remote-navigation or arbitrary URL/file/command API.
Doom is excluded in legacy mode (`capabilities.doom=false`). The additive native
frontend mode is described in [NATIVE_MENU.md](../../docs/NATIVE_MENU.md); it
enables a fixed native Doom lifecycle through the same backend. New routes
require rebuilding/deploying the backend before they are live.

## Discovery and state

| Method | Endpoint | Parameters / response |
| --- | --- | --- |
| GET | `/api/capabilities` | Supported API families; booleans indicate implemented support, not upstream health |
| GET | `/api/system/status` | `ready`, `frontend:"native"|"legacy"`, `doomRunning`, `mediaBusy`; Doom includes lifecycle initialization/cleanup, and mediaBusy adds active backend sources |
| POST | `/api/system/frontend/prepare` | Native-only `{}`; dismisses only known stock boot alert 36 before input acquisition. Returns `prepared:true,dismissedBootAlert:boolean`; invalid parameters 400, legacy mode 409, incomplete dismissal 502. No arbitrary OSD or device command arguments |
| GET | `/api/state` | Shared playback snapshot, transport availability and Jellyfin auth state |
| GET | `/api/status` | Existing playback status, retained for TV compatibility |
| GET | `/api/settings` | `settings:{jellyfinVideoBitrate:12000000}`, `writable:true`, `writableKeys:["jellyfinVideoBitrate"]`, `qualityLevels`; no credentials |
| POST | `/api/settings` | `{"jellyfinVideoBitrate":8000000|12000000|16000000}`; exactly one numeric field, otherwise 400; persists and applies to next PlaybackInfo |
| GET | `/api/jellyfin/status` | `configured`, `authenticated`; sanitized configuration status |

Discovery example:
```json
{"ok":true,"jellyfin":true,"iptv":true,"youtube":true,"frigate":true,"doom":false,"playback":true,"settings":true,"settingsWritable":true}
```
State example:
```json
{"ok":true,"playing":true,"source":"jellyfin","name":"Alien","title":"Alien","itemId":"abc123","paused":false,"live":false,"elapsed":123,"duration":null,"returnToTv":true,"receiver":true,"receiverHost":"192.168.88.103","user":"viewer","transport":{"stop":true,"pause":true,"resume":true,"seek":true},"jellyfinAuthenticated":true}
```
Idle has `source:null`, `itemId:null`, `title:""`, `elapsed:0`, `paused:false`,
`live:false` and all transport flags false. `duration` is seconds for Jellyfin and YouTube
when known, otherwise null. Elapsed is the existing relay wall-clock estimate,
not a hardware decoder position. Existing receiverHost/user fields contain no authentication tokens.

## Jellyfin (existing routes)

| Method | Endpoint | Parameters / response |
| --- | --- | --- |
| GET | `/api/auth/status` | `authenticated`, `user`, `pending`, `code` (pairing code or null) |
| POST | `/api/auth/start` | `{}`; start Quick Connect; returns pairing `code` |
| GET | `/api/auth/poll` | Poll approval; same auth fields, possibly `expired:true` |
| POST | `/api/auth/logout` | `{}`; clear login |
| GET | `/api/libraries` | `libraries:[{id,name}]` |
| GET | `/api/items` | `parent=<id>` optional, `search=<text>` optional, `limit=1..200` (default 60), `offset>=0`, `videoOnly=1` for flat video list |
| GET | `/api/jellyfin/resume` | `offset>=0`; 60 resumable Movie/Episode/Video items, same `{total,items}` shape |
| POST | `/api/play` | `{"itemId":"abc123","startSeconds":0,"returnToTv":true}`; ID required; optional start 0..604800 seconds; returns `playing,name,itemId` |
| GET | `/art/<itemId>.jpg` | Existing binary artwork resource, not a JSON API |
| GET | `/art/native/<itemId>.jpg` | Backend requests <=512×512 artwork; separate cache, same authentication |

Items example:
```json
{"ok":true,"total":1,"items":[{"id":"abc123","name":"Alien","type":"Movie","year":1979,"runtime":70200000000,"overview":"...","playable":true,"isFolder":false,"childCount":null}]}
```
`runtime` is Jellyfin ticks (10,000,000 per second). Additive `resumeSeconds`
comes from backend UserData conversion. Lists already supply menu
item details; no separate details endpoint is needed. For folders, request
`/api/items?parent=<folderId>`; maintain your own folder stack. Search is
recursive and includes matching containers. Paging uses total and actual item
count: `hasMore = offset + items.length < total`. Artwork can be absent (404).
`returnToTv:true` restores the existing TV browse page after stopping; default
false preserves existing `/api/play` behavior.

## IPTV (existing routes, additional logo/paging fields)

| Method | Endpoint | Parameters / response |
| --- | --- | --- |
| GET | `/api/iptv/status` | Library/session availability: `loaded,channels,active,error` plus worker diagnostics |
| GET | `/api/iptv/groups` | `groups:[{name,label,count}],total` (channel count), `groupCount`; optional `offset`, `limit` (default all, max 10000), `hasMore`. Empty-name display label is `Other channels`; filter using the exact name |
| GET | `/api/iptv/channels` | `group=<exact name>` optional; `query=<text>` optional; `limit=1..60` (default 6), `offset>=0` |
| POST | `/api/iptv/play` | `{"channelId":"ch-..."}`; lookup in trusted playlist; returns playback/session metadata |
| POST | `/api/iptv/stop` | `{}`; stop/cancel IPTV preparation; idempotent if IPTV inactive |

```json
{"ok":true,"channels":[{"id":"ch-0123456789abcdef","name":"News","tvgId":"news","group":"News","logo":"https://example.com/news.png"}],"total":42,"offset":0,"limit":6,"hasMore":true}
```
Channel IDs remain stable while playlist tvg-id and stream URL remain stable.
Search matches name, tvg-id or group; group and query can be combined. Logo is
playlist metadata and can be empty; the API does not fetch arbitrary logos.
Playback supports only the existing HTTP/HTTPS HLS/TS pipeline; unsupported
codecs, encryption, DASH, SRT and other unsupported shapes return errors.

## YouTube (existing routes)

| Method | Endpoint | Parameters / response |
| --- | --- | --- |
| GET | `/api/youtube/status` | `available,active,playing,error` |
| GET | `/api/youtube/search` | `q=<text>` (existing maximum 64 decoded bytes), `page=0..100` (zero based) |
| POST | `/api/youtube/play` | `{"videoId":"jNQXAC9IVRw"}`; exact 11-character ID |
| POST | `/api/youtube/stop` | `{}`; stop/cancel YouTube work; idempotent if inactive |

```json
{"ok":true,"page":0,"hasMore":true,"results":[{"id":"jNQXAC9IVRw","title":"Me at the zoo","channel":"jawed","duration":19}]}
```
Up to six results per page. Duration is seconds or null. No backend thumbnail
proxy is provided; the native shell uses service placeholders. Add a backend
image endpoint before introducing native YouTube thumbnail fetching.
Search currently shares the existing exclusive resolver/session slot: stop
current playback before searching YouTube. Resolver operations can take up to
60 seconds; clients should allow at least 65 seconds for search and longer
for playback preparation. Availability does not guarantee upstream success.

## Frigate

| Method | Endpoint | Parameters / response |
| --- | --- | --- |
| GET | `/api/frigate/cameras` | Existing discovery; only sanitized names and codec availability |
| GET | `/api/frigate/status` | Same discovery/health check via the existing implementation |
| POST | `/api/frigate/play` | `{"cameraId":"driveway"}`; configured camera ID required |

```json
{"ok":true,"cameras":[{"id":"driveway","name":"driveway","stream":"driveway","playable":true,"live":true,"reason":""}]}
```
Cameras without a compatible reachable H.264 restream have `playable:false`
and a reason. No camera URL, upstream config body or credentials are returned.
Frigate is still configured at the existing fixed receiver service address.

## Playback

| Method | Endpoint | Body / behavior |
| --- | --- | --- |
| POST | `/api/playback/pause` | `{}`; Jellyfin relay only; `{"ok":true,"paused":true}` |
| POST | `/api/playback/resume` | `{}`; Jellyfin only; `paused:false`; long pauses restart existing stream at elapsed position |
| POST | `/api/playback/stop` | `{}`; stop currently playing media using existing teardown/return path; `stopped:true` |
| POST | `/api/playback/seek` | `{"seconds":120}` absolute or `{"delta":-30}` relative; may combine; Jellyfin only |
| POST | `/api/transport` | Existing equivalent: `{"action":"pause|resume|stop"}` |
| POST | `/api/seek` | Existing alias for seek |
| POST | `/api/tv/exit` | Existing capability to leave the custom UI and return to stock TV; not needed for browsing |

In `--native-frontend` mode the backend suppresses its Druid launcher/polling
and ITV return chain. `/api/tv/exit` acknowledges native surface dismissal.
Decoder stop goes through fixed stock control in the backend instead of
synthetic transport keys which the native frontend owns. Legacy paths remain.

Normal Jellyfin upstream EOF does not immediately end playback: the receiver
can still have buffered media. State remains `playing:true,draining:true` until
estimated runtime (including seek offset and prior pauses) plus a 15-second
decoder startup allowance has elapsed, then uses the existing 8-second UI-return
delay. Unknown runtime uses a bounded 15-second EOF allowance. This is an
estimate, not a hardware playback-completion callback. Stop/seek can cancel the
drain. Pause/resume return 409 while draining; consult `transport` flags.

Pause/resume are idempotent and report actual pause state. Seek restarts the
existing Jellyfin transcode, rather than simulating remote keys. Seconds must
be numeric 0..604800; delta numeric -604800..604800; negative resulting targets
clamp to zero. IPTV, Frigate and YouTube support stop only: pause/resume/seek
return errors instead of ignored success. Use the source-specific stop routes
to cancel pending IPTV/YouTube preparation before playback exists. Stop during
idle returns 409 on the new generic route.

## Native Doom lifecycle

| Method | Endpoint | Behavior |
| --- | --- | --- |
| POST | `/api/doom/start` | `{}`; native mode only; fixed binary/WAD, readiness handshake and lifecycle lock; returns `running:true`; requires backend arming marker |
| GET | `/api/doom/status` | Existing fields plus `native`; `running` includes native initialization/cleanup while lifecycle lock is held |
| POST | `/api/doom/stop` | `{}`; TERM and wait for clean shutdown; `running:false` only after native cleanup; errors on incomplete shutdown |

Caller-controlled executable paths/arguments are not accepted. Executable PID
identity is verified before signalling. Legacy canvas/frame/input routes stay
available. See NATIVE_MENU.md for fixed asset layout and frontend key/surface
handoff; physical lifecycle acceptance is still pending.

## Errors, compatibility and deployment

- 400: malformed/non-object JSON, invalid IDs, invalid transport action, seek values or quality selection.
- 404: unknown endpoint or IPTV channel ID; absent artwork.
- 409: new transport routes have no active playback or source does not support the action; seek on non-Jellyfin sources.
- 405: unsupported HTTP method.
- 502: existing integration errors, including missing Jellyfin login, unavailable
  upstream/resolver, unknown configured camera, unsupported streams, conflicts
  reported by legacy integration functions, or playback preparation failure.
- 413/431: oversized bodies/headers. No API errors contain HTML.

Example: `{"ok":false,"error":"seek unsupported for this source"}`.
Status responses can contain an `error` diagnostic string even with `ok:true`:
that means status was successfully read, not that the upstream is healthy.
Legacy `/api/transport` integration failures retain HTTP 502; new explicit
transport routes preflight idle/unsupported actions with 409.

Existing TV checkpoint APIs `/api/tv/state`, `/api/iptv/state`,
`/api/youtube/state` (GET/POST), telemetry `/api/tv/event` and input bridge
`/api/tv/input` are preserved. Native clients need none of these to browse or
play; their own selection is independent of the DOM. No frontend logic moved.
The DVR OSK and normal-browser text field behavior are unchanged.

LAN API has the existing unauthenticated access model. Keep it on a trusted
LAN; do not publish it to the Internet. Quality writes only `config/playback-quality.json`; credentials, upstream addresses,
and source state are not writable through settings.

Native-client limits: duration can be unknown; elapsed is approximate; YouTube search requires the playback slot;
only Jellyfin video quality is editable; Doom is native-mode-only. These mirror current capabilities.
Build with the existing MIPS toolchain and deploy through the existing guarded
deployment script when receiver playback is idle; tests below use local mocks.

```sh
python3 test/capability_api_test.py
python3 test/jellyfin_relay_failure.py
bash test/run_host_tests.sh
python3 test/youtube_integration.py
cc -O1 -o /tmp/hr54-iptv-fetch-host ../../modules/iptv/fetch.c -lcurl
python3 test/iptv_integration.py
node test/media_hub_test.js
```

The existing persistent launcher automatically selects native mode when the
sibling `native-menu/ENABLED` and `ACTIVATED` markers and fixed executable
bootstrap helper exist. Its listener is created before bootstrap starts; the
frontend remains a localhost API consumer. `mediaBusy` is the backend-owned idle
gate for a boot-policy reload; clients must treat an absent field as unknown.
