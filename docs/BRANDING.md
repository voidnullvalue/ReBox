# ReBox artwork and background update

The mascot reference is `docs/rebox-mascot.png`, retrieved from
https://github.com/voidnullvalue/ReBox/blob/main/docs/rebox-mascot.png.
The generated transparent masters and eleven vendor-sized exports are in
`receiver/payload/hr54-persist/branding/`. Both masters were produced with the
built-in imagegen tool, using the mascot as the reference.

The prepared `receiver/branding/23_6933_6840.squashfs` replaces only the eleven
old artwork PNGs in the existing receiver's asset-23 pack. All other pack files
are unchanged. The existing genuine asset-7 signature anchor is retained.
`tools/update-receiver-branding.sh` is a guarded, receiver-specific update for
192.168.88.103's existing custom asset-23 deployment, not a stock installer.
Previous plugin and recovery snapshots remain saved on the receiver.

The UI background retains the existing smoke field and uses a contrast mapping
of `5*256 + 4*(smoke-vignette)`, clamped to grayscale 0–18. Its darkest folds
are pure black, with a modest increase in the highlights. The vignette keeps
fractional fixed-point precision to remove the previous abrupt dark side strips.
Coordinate warp, interpolation and all other UI drawing remain unchanged.
The smoke loop is now 120 seconds rather than 180 seconds; its update rate
remains 5 Hz. `docs/rebox-home-preview.png` shows the
production renderer at 45s.
The stock installation payload retains the snapshot's legacy frontend with the
rendering fixes, paired with its existing monolithic API. The current
module-aware frontend is built from main and saved as
`receiver/runtime/hr54-ui`. Both use the same smoke renderer.

Restoring the old monolithic backend regressed provider playback on the
development receiver. `tools/rejoin-provider-runtime.sh` then restored its
already-installed October 7 provider runtime and installed the freshly built
module-aware frontend, while retaining the accepted artwork, smoke tuning,
accounts, playlists and existing policy/broker. It checks the precise receiver
snapshot, core and staged UI hashes, and rejoins the loaded policy without
reloading middleware. Its backups are under
`/var/hr54-persist/backup/provider-repair-20261007.OD8pFp`.

Opening any receiver Home module now checks live core ownership, cleanly stops
previous media playback or a native app, and rechecks idle readiness before
entering the chosen module. A failed stop retains Home and displays a retry
message. The selected module ID remains fixed while the handoff runs; MENU,
BACK and EXIT cancel a pending opening. Tests cover media-to-browser,
native-to-browser, media-to-native, stale playback status, and failed stops.

The user confirmed moving Frigate camera video through the restored
WebSocket/fMP4-to-MPEG-TS relay, then confirmed that selecting Doom while
the camera was playing stopped the camera and opened Doom automatically. IPTV/YouTube playback rechecks were interrupted
by a user-confirmed network outage; this is not a claim of renewed acceptance
for those sources. Builds, UI/module tests, stock payload checks and branded
asset checks passed. The receiver's normal reboot was verified before the
provider runtime rejoin; that final rejoin was live, not another reboot.

The receiver display preference was changed from 480p to EDID-supported
1080p at 16:9 to address side bars; the prior settings are saved under
`/var/hr54-persist/backup/restore-from-ReBox-20261007/display-before.txt`.
The receiver returned to 480p after reboot, despite retaining 1080p in its
enabled preferences. The native UI and all eleven mounted branded images
passed post-reboot readiness and integrity checks. The user accepted the live
smoothed vignette before reboot; post-reboot physical playback acceptance remains
separate from these checks.

## Final generation prompts

### Stacked master

Use case: logo-brand. Asset type: ReBox receiver boot logos and screensaver graphics. Input image 1 is the approved ReBox mascot reference. Create a polished production-ready transparent PNG brand lockup: preserve the same cute dark charcoal set-top-box mascot, exact proportions and facial identity, blue eyes and blue/white shoes, green power light, thumbs-up pose, and tiny rebox label on its front. Place a clean highly legible wordmark 'ReBox' centered below the mascot, with 'Re' white and 'Box' vivid blue matching the shoes, modern bold rounded sans serif. Keep character and wordmark separated by modest clear space. Centered compact composition with transparent background, generous 8% transparent margins. No shadows outside the mascot, no panel, no background, no other text. Preserve the mascot artwork faithfully; do not invent another character.

### Horizontal master

Use case: logo-brand. Create a very wide horizontal ReBox receiver boot logo lockup on genuinely transparent background. Aspect ratio about 3.5:1, suitable for fitting a 702x197-pixel space. Input image is approved mascot reference; preserve its exact cute charcoal receiver character, thumb-up pose, face, blue eyes, blue white sneakers and green status light. The full mascot occupies the left quarter of the composition. To its right place very large clean bold rounded sans-serif wordmark 'ReBox', with 'Re' bright white and 'Box' saturated blue matching the shoes. Align mascot and wordmark vertically centered. Mascot fully visible, compact clear spacing, no clipping. Minimal 5% transparent outer margins. No shadow, no glow, no texture or background, no other text or decorative objects. Make the wordmark clearly legible at small TV resolution.
