# Radio module

Module-owned catalog, genre/search navigation, station checks and streaming.
Core owns the vendor decoder. See `docs/RADIO-AND-SURFING.md` at the repository
root for controls, measurements and acceptance.

`build-receiver.sh` builds the module and its audio helper for big-endian MIPS32.
It unpacks the included FFmpeg 8.0 source archive into the ignored build directory.
Set `HR54_ZIG` and optionally `REBOX_RADIO_FFMPEG` to an existing FFmpeg tree.
`build-audio.sh` may also be run directly inside an unpacked FFmpeg tree.

The normal runtime builder includes the manifest, icon, black-video asset,
Python worker, encoder and catalog defaults. The worker uses the standard
receiver distribution's bundled Python 3.14 runtime and CA certificates.
Existing private catalog data is preserved by the module SDK's default seeding.

Refresh package defaults with:

```sh
python3 update-playlists.py default-data/radio
```

The upstream URL catalog is CC0; its license and per-playlist hashes are included.
The radio SVG/PNG is original ReBox artwork. `assets/black.ts` is an original
one-second, 320×180, baseline H.264 black clip generated with FFmpeg/libx264:

```sh
ffmpeg -f lavfi -i color=c=black:s=320x180:r=25 -t 1 -an \
  -c:v libx264 -profile:v baseline -g 25 -pix_fmt yuv420p -f mpegts assets/black.ts
```

The audio helper uses the included FFmpeg libraries; it does not use networking
or an external conversion server. Package/build caches are excluded from Git.
