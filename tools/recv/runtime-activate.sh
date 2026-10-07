#!/bin/sh
# Explicit, guarded live migration. Stock middleware/policy/broker are untouched.
set -eu
umask 077
[ "${1:-}" = ACTIVATE_RUNTIME_MODULES ] || exit 2
base=/var/hr54-persist
menu=$base/native-menu
runtime=$base/rebox
upgrade=$base/rebox-upgrades/runtime-20261007-v2
backup=$(cat "$base/rebox-runtime-rollback-path")
[ "$backup" = "$upgrade/backup" ] && [ ! -L "$backup" ] || exit 1
[ "$(cat "$upgrade/staging.complete")" = staged ]
(cd "$base" && md5sum -c "$upgrade/activation-files.md5")
(cd "$backup" && md5sum -c rollback.md5)
# Require the existing loaded policy so bootstrap cannot reload middleware.
boot=$(cat /proc/sys/kernel/random/boot_id)
policy=$(cat "$menu/policy.md5")
case "$(cat "$menu/loaded-boot")" in "$boot:$policy:"*:native) :;; *) echo 'Loaded policy does not match this boot'; exit 1;; esac
[ "$(md5sum /opt/dtv/dtv.car | cut -d' ' -f1)" = "$policy" ]
[ "$(cat /var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str)" = false ]
[ "$(md5sum /opt/dtvwm/lib/libdtvwm.so | cut -d' ' -f1)" = 0222999c41c9a57dabd8c9b3d714b69e ]
[ "$(md5sum /opt/dtvwm/bin/dtvwm | cut -d' ' -f1)" = fed62d04663412e60388ac09a83cbc05 ]
[ "$(md5sum /opt/dtv/dtv.car | cut -d' ' -f1)" = "$(cat "$menu/policy.md5")" ]
"$menu/hr54-ui" --check-api --idle
"$upgrade/stage/hr54-ui" --port 8132 --check-api --idle
stop_pid() {
    pid=$1; expected=$2
    case "$pid" in ''|*[!0-9]*) exit 1;; esac
    [ "$pid" -gt 1 ] && [ "$(readlink "/proc/$pid/exe")" = "$expected" ] || exit 1
    kill -TERM "$pid"
    n=0
    while [ -d "/proc/$pid" ]; do
        grep -q '^State:.*Z' "/proc/$pid/status" && break
        n=$((n+1)); [ "$n" -le 20 ] || { echo 'Owned process did not stop cleanly'; exit 1; }; sleep 1
    done
}
stop_pid "$(cat "$upgrade/trial-core.pid")" "$runtime/bin/reboxd"
# Stop the UI supervisor, which gracefully releases its child surface/consumer.
pid=$(cat /tmp/hr54-ui-supervisor.lock/pid)
case "$pid" in ''|*[!0-9]*) exit 1;; esac
case "$(readlink "/proc/$pid/exe")" in /bin/bash|/bin/busybox) :;; *) exit 1;; esac
grep -q "$menu/supervisor.sh" "/proc/$pid/cmdline" || exit 1
kill -TERM "$pid"
n=0
while [ -e "$menu/ui.pid" ] || [ -d /tmp/hr54-ui-supervisor.lock ]; do
    n=$((n+1)); [ "$n" -le 15 ] || { echo 'UI ownership release incomplete'; exit 1; }; sleep 1
done
stop_pid "$(cat "$base/jellyfin/jf.pid")" "$base/jellyfin/bin/hr54-jf"
# Atomic replacement only after old executables have stopped. Backups remain.
cp "$upgrade/stage/hr54-ui" "$menu/hr54-ui.next"
chmod 700 "$menu/hr54-ui.next"
mv "$menu/hr54-ui.next" "$menu/hr54-ui"
cp "$upgrade/stage/legacy-runtime-entry.sh" "$base/jellyfin/bin/hr54-jf.next"
chmod 700 "$base/jellyfin/bin/hr54-jf.next"
mv "$base/jellyfin/bin/hr54-jf.next" "$base/jellyfin/bin/hr54-jf"
cp "$upgrade/stage/rebox-start.sh" "$base/rebox-start.sh.next"
chmod 700 "$base/rebox-start.sh.next"
mv "$base/rebox-start.sh.next" "$base/rebox-start.sh"
cp "$upgrade/stage/runtime-rollback.sh" "$base/rebox-runtime-rollback.sh"
chmod 700 "$base/rebox-runtime-rollback.sh"
: > "$base/REBOX_ENABLED"
sync
"$base/rebox-start.sh"
n=0
while ! "$menu/hr54-ui" --check-api; do
    n=$((n+1)); [ "$n" -le 30 ] || { echo 'New API readiness failed; retain logs and invoke guarded rollback'; exit 1; }; sleep 1
done
printf 'activated\n' > "$upgrade/activation.complete"
echo 'Runtime API activated. Physical display/input/media acceptance is pending.'
