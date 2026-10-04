# hr54-ui

Receiver-native API client and original media-center shell. Architecture,
contracts, validation status and the outstanding boot/MENU gates are in
[docs/NATIVE_MENU.md](../docs/NATIVE_MENU.md).

```sh
make host receiver previews test
```

Host previews use the production C renderer. Fixture data is linked only into
the host executable. `previews/contact-sheet.png` includes every requested screen.

Host interactive checks against a local backend:

```sh
build/hr54-ui-host --port 8130 --output build/current.rgba
```

Keys: `w/a/s/d`, SPACE select, `b` back, `g` guide, `i` info, `p` pause,
`x` stop, `f/r` seek, `q` quit. Host fixtures do not prove receiver integration.

`--manual` on the receiver excludes GUIDE/MENU registration for supervised
validation. It cannot meet the permanent-menu requirement. The default receiver
mode requires MENU and fails closed on rejection. `--guide` additionally requests
GUIDE. The temporary MENU-only stock map overlay now allows the receiver test
to run in default mode; GUIDE retains its stock owner. The policy preparation
and receiver test are in `tools/menu-policy.py` and `tools/test-menu-policy.sh`.
They are deployment tooling, separate from the API-client frontend. Receiver
display tests run without a timeout and stay active until the user acknowledges
them. The test overlay is removed by reboot; permanent boot activation uses the existing backend launcher plus the guarded
`bootstrap.sh` policy reload.

The user acknowledged the visible MENU repair and requested permanent deployment.
`ENABLED` and `ACTIVATED` select the native boot path; `ACTIVATED` records that
instruction, not acceptance of the entire feature matrix. The fixed supervisor
waits for native API readiness and limits crash restarts to three attempts.
Deployment and rollback scripts are in `tools/`; keep the legacy HTML and saved
backend until the remaining receiver acceptance checks are complete.

`--check-api [--idle]` checks typed native readiness without input or graphics.
`--stop-media` invokes the existing backend stop API for maintenance. Receiver
`--probe-handoff` checks shell/video/stock key-map transitions without graphics.


The current design is minimalist and XMB-inspired: official Jellyfin SVG,
Overpass typography, antialiased icon/artwork filtering, neutral flat surfaces,
and subtle home-only grayscale plasma (5 Hz, 180-second loop). Font/art licenses
and reproduction instructions are in [assets/SOURCES.md](assets/SOURCES.md).
The production-renderer motion preview is `previews/plasma-home.gif`.

`--still` freezes decorative motion for host comparisons. `--preview-ms N`
selects a deterministic plasma phase for PNG generation. `--start-hidden`
keeps current playback visible until MENU is pressed or playback ends; it is
intended for frontend-only updates during playback. Playback ending restores
the saved browse/details selection, or Home when playback predates the UI
process. Normal boot continues to show Home.
