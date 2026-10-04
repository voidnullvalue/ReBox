# Doom 1.9 shareware

The native menu uses the original id Software Doom shareware IWAD,
`doom1.wad`, not Freedoom or the registered game's data.

Original distribution: `idstuff/doom/doom19s.zip` from the /idgames archive.
Downloaded from its [YouFailIt archive mirror](https://youfailit.net/pub/idgames/idstuff/doom/doom19s.zip).
The complete original distribution is retained at `../doom19s.zip`.
`README.TXT` is the original release documentation; retain it with the assets.
This game data is shareware, not an open-licensed replacement IWAD.

The installer is split into `DOOMS_19.1` and `.2`; concatenation produces the
self-extracting ZIP. The WAD was extracted without executing DOS programs.

Verified asset:

* Size: 4,196,020 bytes.
* IWAD directory: 1,264 lumps.
* Maps: E1M1–E1M9; no E2M1 or FREEDOOM marker.
* MD5: `f0cefca49926d00903cf57551d901abe`.
* SHA-256: `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`.

Install at `$JF_PERSIST_ROOT/doom/data/doom1.wad`, keeping the native engine at
`doom/bin/hr54-doom-native`. Launch remains behind `/api/doom/start`; changing
the game data does not add source or process integration to the frontend.
Earlier Freedoom display-proof assets remain historical test data and are not
the native-menu game's default.
