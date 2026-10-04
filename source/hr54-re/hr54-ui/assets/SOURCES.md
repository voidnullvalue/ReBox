# Native UI assets

Overpass is bundled from [Google Fonts' Overpass directory](https://github.com/google/fonts/tree/main/ofl/overpass). The original variable font is `Overpass.ttf`, licensed under the SIL Open Font License in `OFL.txt`. UI/body glyphs use weight 400; large titles use weight 300. Glyphs are baked at 3× resolution and filtered into grayscale coverage masks. No font download occurs on the receiver. Characters outside Overpass's supported scripts retain the existing bundled Noto Sans fallback (`NotoSans-Regular.ttf`, `NotoSans-OFL.txt`).

The Jellyfin icon is the official, unmodified color-on-dark SVG from the [Jellyfin UX repository](https://github.com/jellyfin/jellyfin-ux/blob/master/logos/SVG/jellyfin-icon--color-on-dark.svg), by the Jellyfin project contributors, licensed under [CC BY-SA 4.0](https://github.com/jellyfin/jellyfin-ux/blob/master/LICENSE). Its path and gradient are preserved; only rasterization, antialiasing, sizing and focus opacity are applied for display. The complete license is bundled as `JELLYFIN-LICENSE.txt`. Jellyfin's mark identifies that service and does not imply endorsement.

The IPTV, camera, video and game-controller SVGs are original artwork for this shell. All five icons are rasterized at 4× size and Lanczos-filtered to a transparent 128px atlas. Runtime scaling uses premultiplied-alpha filtering and a bounded cache. Original downloads and SHA-256 hashes are recorded in `sources.json`.

Rebuild embedded assets with a host environment containing Pillow, fonttools and CairoSVG:

```sh
python3 tools/bake_font.py
python3 tools/bake_icons.py
```

Generated C atlases are committed with the source; the receiver needs none of these build libraries. The plasma background is generated procedurally by the production renderer with a baked integer sine table, rather than a video, downloaded wallpaper or external service.
