# Bundle validation — 2026-10-04

These checks apply to this packaged snapshot, not an unobserved second receiver.

Passed:

- Exact accepted UI, broker, backend, playURL wrapper and native Doom MD5s;
  exact installed Android APK SHA-256 and arm64-v8a native library presence.
- Live CAR policy retrieved through a whitelisted public-runtime export;
  offline hash-guarded regeneration from exact original stock CAR produced
  an identical file, MD5 `c1b9e5ba4546c42f799cf580f048afb1`.
- Genuine asset-7 signature checked using exact vendor public material:
  key type 0; genuine image/declared size both 2,027,520 bytes.
- New portable plugin preserves genuine indexer executable (`indexer.real`)
  and libmp4lib.so byte-for-byte. Its feature/version config is preserved from
  the hardware-proven v6 hook (version 6933, minimum stack 6840), not copied
  unchanged from the genuine future-version image.
- Portable indexer offline branch executed with vendor BusyBox ash under
  MIPS emulation in an isolated writable `/var`: exit 22, root proof and
  expected test marker. This is an offline launcher check, not a hardware boot.
- Repaired source host UI/reactor/input/boot/rollback tests and backend
  media/lifecycle/quality/native-I-frame tests passed from the bundled tree.
- Read-only big-endian VM stock audit passed against the original stock HDD
  partition image. That image was not written.
- Full guest install into a disposable qcow2 overlay backed by that stock image:
  root-owned initramfs/payload installation, all payload MD5s passed; XFS
  unmounted cleanly; `REBOX_CLONE_INSTALL_PASS`.
- Fresh read-only VM verified the installed payload, config, both plugin files,
  retained genuine anchor and absent native activation flag; XFS unmounted
  cleanly; `REBOX_INSTALLED_READONLY_PASS`.
- Isolated stock activation/rollback tests rejected wrong firmware, invalid
  PID, media busy and malformed readiness BEFORE markers/process termination.
  Success saved original CAR/registry/config, terminated only the exact owned
  test process, started the mock launcher, and restored stock presentation
  without deleting snapshots.
- Installer rejected physical/non-regular and wrong-size inputs before VM
  creation or writes. Scripts passed syntax checks. All runtime account/state/
  cache directories are empty, cookies/keys/phone settings are absent, native
  activation is absent, and all shipped symlinks resolve inside the bundle.

The final portable stock plugin SHA-256 is
`8a62998d81986ff12af6bb166c78b60c377eaae3a5c8f24d78d141f0d481f10e`.
Core runtime binaries remain exactly the physically accepted repaired versions.
See SHA256SUMS for all other identities.

The raw-clone host installer was exercised in read-only audit mode; its guest
write/verify path was exercised on a copy-on-write overlay to avoid allocating
or writing the original full partition. No physical disk restore, second-box
native activation/HDMI acceptance or second-phone request is claimed.
No reboot, media operation or service/configuration replacement was performed
on the live receiver while assembling this bundle; only public-file exports
were staged for copying. Private receiver backups and test overlays are outside
ReBox and are not included in the shareable folder.

Rerunnable checks:

```sh
sha256sum -c SHA256SUMS
python3 tools/check-bundle.py
python3 tools/test-install-lifecycle.py
```

Source builds/tests create outputs under `source/`; run integrity verification
before building or copy source to a separate working directory. Build outputs
will add/change files and must not be confused with the shipped runtime payload.
Do not regenerate integrity manifests merely to silence an unexplained mismatch.
