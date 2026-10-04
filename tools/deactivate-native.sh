#!/bin/sh
# Restore the backed-up stock registry; a normal reboot applies stock policy.
# Keeps root access, payload, user configuration and all rollback data.
set -eu
umask 077
base=/var/hr54-persist
menu=$base/native-menu
registry=/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID
[ "${1:-}" = RESTORE_STOCK_PRESENTATION ] || exit 2
[ "$(id -u)" = 0 ] || exit 1
"$menu/hr54-ui" --check-api --idle || { echo 'Stop playback/Doom first' >&2; exit 1; }
backup=$(cat "$menu/rebox-rollback-path")
case "$backup" in "$menu"/backup/rebox-stock-activation.*) :;; *) exit 1;; esac
[ -d "$backup" ] && [ ! -L "$backup" ]
[ "$(cat "$backup/Context-Main.str")" = com.directv.druid.DruidMain ]
[ "$(cat "$backup/Auto-Start.str")" = true ]
[ "$(md5sum "$backup/dtv.car.stock" | cut -d' ' -f1)" = 882124071cfe3ac1740af18161995dcf ]
# Move activation flags into a NEW backup instead of deleting them.
saved=$(mktemp -d "$menu/backup/rebox-disabled.XXXXXX")
for flag in ENABLED ACTIVATED BROKER_ENABLED STOCK_UI_SUPPRESSION; do
    [ ! -f "$menu/$flag" ] || mv "$menu/$flag" "$saved/$flag"
done
cp -p "$backup/Auto-Start.str" "$registry/.Auto-Start.rebox-restore"
mv "$registry/.Auto-Start.rebox-restore" "$registry/Auto-Start.str"
sync
echo 'Stock Auto-Start restored. Reboot normally to release native processes and RAM-only policy mounts.'
echo 'Root hook, payload, accounts and backups retained. Complete removal is in docs/ROLLBACK.md.'
