#!/bin/sh
# Run ON THE RECEIVER, only after the stock firmware preflight passes.
set -eu
umask 077
base=/var/hr54-persist
menu=$base/native-menu
registry=/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID
[ "${1:-}" = ACTIVATE_NATIVE_ON_SUPPORTED_STOCK_HR54 ] || {
    echo 'Usage: activate-native.sh ACTIVATE_NATIVE_ON_SUPPORTED_STOCK_HR54' >&2; exit 2;
}
[ "$(id -u)" = 0 ] || exit 1
[ ! -e "$menu/ACTIVATED" ] && [ ! -e "$menu/ENABLED" ] || {
    echo 'Already activated; this is not an update installer' >&2; exit 1;
}
cd "$base"
md5sum -c payload.md5
md5sum -c stock-firmware.md5
[ "$(cat "$registry/Context-Main.str")" = com.directv.druid.DruidMain ]
[ "$(cat "$registry/Auto-Start.str")" = true ]
pid=$(cat "$base/jellyfin/jf.pid")
case "$pid" in ''|*[!0-9]*) exit 1;; esac
[ "$(readlink "/proc/$pid/exe")" = "$base/jellyfin/bin/hr54-jf" ]
# The native CLI intentionally rejects frontend=legacy. Before activation this
# new stock install runs --no-launcher with legacy presentation, so validate the
# exact known backend readiness envelope instead of weakening the native CLI.
idle_before_activation() {
    readiness=$(wget -qO- http://127.0.0.1:8130/api/system/status) || exit 1
    case "$readiness" in
        '{"ok":true,"ready":true,"frontend":"legacy","doomRunning":false,"mediaBusy":false}') :;;
        *) echo 'Pre-activation API is unavailable, malformed, native already, or media busy' >&2; exit 1;;
    esac
}
idle_before_activation
backup=$(mktemp -d "$menu/backup/rebox-stock-activation.XXXXXX")
cp -p "$registry/Auto-Start.str" "$registry/Context-Main.str" "$backup/"
cp -p /opt/dtv/dtv.car "$backup/dtv.car.stock"
cp -p "$base/rebox.conf" "$backup/rebox.conf"
printf '%s\n' "$backup" > "$menu/rebox-rollback-path"
idle_before_activation
# Nothing changes the stock registry until the guarded bootstrap runs.
: > "$menu/STOCK_UI_SUPPRESSION"
: > "$menu/BROKER_ENABLED"
: > "$menu/INPUT_TRACE"
printf 'ReBox stock installation; physical/API acceptance still required\n' > "$menu/ACTIVATED"
: > "$menu/ENABLED"
kill -TERM "$pid"
n=0
while kill -0 "$pid" 2>/dev/null; do
    grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
    n=$((n+1)); [ "$n" -lt 15 ] || { echo 'Clean API shutdown incomplete; use rollback' >&2; exit 1; }
    sleep 1
done
"$base/rebox-start.sh"
sync
printf 'Activation started; backup=%s\nCheck bootstrap.log and perform physical acceptance.\n' "$backup"
