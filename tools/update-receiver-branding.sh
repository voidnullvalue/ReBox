#!/bin/sh
# Run on the existing supported receiver; preserve all prior plugin snapshots.
set -eu
umask 077
[ "${1:-}" = INSTALL_REBOX_BRANDING_ON_SUPPORTED_RECEIVER ] || exit 2
base=/var/hr54-persist
work=$base/backup/restore-from-ReBox-20261007/branding
plugins=/var/network/plugins
staged=/var/hr54-transfer/rebox-branding-plugin23-20261007.squashfs
helper=/var/hr54-transfer/rebox-verify-assets-20261007.sh
[ -d "$work" ] && [ ! -L "$work" ]
[ "$(md5sum "$helper" | cut -d' ' -f1)" = 1e562d274997f094c0baa226a4fdde4f ]
[ "$(md5sum "$staged" | cut -d' ' -f1)" = 610fe6787bec721e293daae6e61a0d32 ]
[ "$(md5sum "$plugins/23_6933_6840.squashfs" | cut -d' ' -f1)" = d714a4763be8d6f0a9bc0d5a0567b901 ]
[ "$(md5sum "$plugins/7_6932_6932.squashfs" | cut -d' ' -f1)" = fcf1d2be9d0f9c680e35759bf13c81a2 ]
[ "$(md5sum "$work/23_6933_6840.squashfs.before" | cut -d' ' -f1)" = d714a4763be8d6f0a9bc0d5a0567b901 ]
# Retain the existing genuine signature anchor and verifier checks.
/opt/sig/bin/sigtst "$plugins/23_6933_6840.sig"
cd "$base"
sed '\|  /opt/dtv/dtv.car$|d' stock-firmware.md5 > "$work/firmware.md5"
md5sum -c "$work/firmware.md5"
"$base/native-menu/hr54-ui" --check-api --idle
[ ! -e "$work/hr54-verify-assets.before" ]
cp -p "$base/bin/hr54-verify-assets" "$work/hr54-verify-assets.before"
[ ! -e "$base/backup/23_6933_6840-rebox.squashfs" ]
cp "$staged" "$base/backup/23_6933_6840-rebox.squashfs"
cp -p "$plugins/23_6933_6840.sig" "$base/backup/23_6933_6840-rebox.sig"
cp "$staged" "$plugins/.23_6933_6840.squashfs.rebox-next"
chmod 644 "$plugins/.23_6933_6840.squashfs.rebox-next"
mv "$plugins/.23_6933_6840.squashfs.rebox-next" "$plugins/23_6933_6840.squashfs"
cp "$helper" "$base/bin/hr54-verify-assets.rebox-next"
chmod 700 "$base/bin/hr54-verify-assets.rebox-next"
mv "$base/bin/hr54-verify-assets.rebox-next" "$base/bin/hr54-verify-assets"
md5sum "$plugins/23_6933_6840.squashfs" "$base/backup/23_6933_6840-rebox.squashfs" > "$work/installed.md5"
/opt/sig/bin/sigtst "$plugins/23_6933_6840.sig"
sync
printf 'REBOX_BRANDING_INSTALLED_FOR_NEXT_BOOT\n' > "$work/result"
cat "$work/result"
