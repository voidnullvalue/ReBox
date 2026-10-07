#!/bin/sh
# Restore pre-upgrade code/startup only. Keep all accounts, playlists and new data.
set -eu
umask 077
[ "${1:-}" = RESTORE_PRE_MODULE_BACKEND ] || exit 2
base=/var/hr54-persist
menu=$base/native-menu
runtime=$base/rebox
backup=$(cat "$base/rebox-runtime-rollback-path")
case "$backup" in "$base"/rebox-upgrades/*/backup) :;; *) exit 1;; esac
[ -d "$backup" ] && [ ! -L "$backup" ] || exit 1
(cd "$backup" && md5sum -c rollback.md5)
# Require the existing loaded policy so bootstrap cannot reload middleware.
boot=$(cat /proc/sys/kernel/random/boot_id)
policy=$(cat "$menu/policy.md5")
case "$(cat "$menu/loaded-boot")" in "$boot:$policy:"*:native) :;; *) echo 'Loaded policy does not match this boot'; exit 1;; esac
[ "$(md5sum /opt/dtv/dtv.car | cut -d' ' -f1)" = "$policy" ]
[ "$(cat /var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str)" = false ]
# Native locks and the active decoder must be idle before changing ownership.
"$menu/hr54-ui" --check-api --idle || { echo 'Stop media/native apps first; rollback has not changed files'; exit 1; }
stop_ui() {
    lock=/tmp/hr54-ui-supervisor.lock
    pid=$(cat "$lock/pid" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) pid=;; esac
    if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
        case "$(readlink "/proc/$pid/exe")" in /bin/busybox|/bin/bash) :;; *) exit 1;; esac
        grep -q "$menu/supervisor.sh" "/proc/$pid/cmdline" || exit 1
        kill -TERM "$pid"
    fi
    n=0
    while [ -e "$menu/ui.pid" ] || [ -d "$lock" ]; do
        n=$((n+1)); [ "$n" -le 15 ] || { echo 'UI clean shutdown incomplete'; exit 1; }; sleep 1
    done
}
stop_ui
pid=$(cat "$runtime/reboxd.pid")
case "$pid" in ''|*[!0-9]*) exit 1;; esac
[ "$pid" -gt 1 ] && [ "$(readlink "/proc/$pid/exe")" = "$runtime/bin/reboxd" ] || exit 1
kill -TERM "$pid"
n=0
while [ -d "/proc/$pid" ]; do
    grep -q '^State:.*Z' "/proc/$pid/status" && break
    n=$((n+1)); [ "$n" -le 20 ] || { echo 'Core clean shutdown incomplete'; exit 1; }; sleep 1
done
cp -p "$backup/hr54-ui" "$menu/hr54-ui.restore"
mv "$menu/hr54-ui.restore" "$menu/hr54-ui"
cp -p "$backup/hr54-jf" "$base/jellyfin/bin/hr54-jf.restore"
mv "$base/jellyfin/bin/hr54-jf.restore" "$base/jellyfin/bin/hr54-jf"
if [ -f "$backup/rebox-start.sh" ]; then
    cp -p "$backup/rebox-start.sh" "$base/rebox-start.sh.restore"
    mv "$base/rebox-start.sh.restore" "$base/rebox-start.sh"
else
    [ ! -f "$base/rebox-start.sh" ] || mv "$base/rebox-start.sh" "$backup/rebox-start.rolled-back.sh"
fi
if [ -f "$backup/REBOX_ENABLED" ]; then
    cp -p "$backup/REBOX_ENABLED" "$base/REBOX_ENABLED"
else
    [ ! -f "$base/REBOX_ENABLED" ] || mv "$base/REBOX_ENABLED" "$backup/REBOX_ENABLED.rolled-back"
fi

sync
"$backup/legacy-launch.sh"
(trap '' HUP; exec "$menu/bootstrap.sh") >> "$menu/rollback-bootstrap.log" 2>&1 < /dev/null &
echo 'Pre-module backend and UI restored; module packages/data and backups retained.'
