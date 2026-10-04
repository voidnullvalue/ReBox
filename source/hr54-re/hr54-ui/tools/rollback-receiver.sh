#!/bin/sh
# Explicit rollback of the native menu; retain API settings/auth and HTML.
set -eu
root=/var/hr54-persist/native-menu
backend=/var/hr54-persist/jellyfin
backup=$root/backup
stop_owned() {
    pid=$(cat "$1" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) return 0;; esac
    [ -d "/proc/$pid" ] || return 0
    [ "$(readlink "/proc/$pid/exe")" = "$2" ] || { echo 'PID identity mismatch'; exit 1; }
    kill -TERM "$pid"
    count=0
    while kill -0 "$pid" 2>/dev/null; do
        grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
        count=$((count+1)); [ "$count" -lt 10 ] || { echo 'Clean shutdown incomplete'; exit 1; }
        sleep 1
    done
}
[ -x "$backup/hr54-jf" ] || { echo 'Missing original backend backup'; exit 1; }
# Refuse a middleware reload during an API-owned source or native game.
"$root/hr54-ui" --check-api --idle || { echo 'Stop active media through the API before rollback; no reload performed'; exit 1; }
rm -f "$root/ENABLED" "$root/ACTIVATED"
# Stop the shell supervisor by its fixed script command, then its UI if needed.
pid=$(cat /tmp/hr54-ui-supervisor.lock/pid 2>/dev/null || :)
case "$pid" in ''|*[!0-9]*) :;; *)
    if [ -d "/proc/$pid" ]; then
        grep -q "$root/supervisor.sh" "/proc/$pid/cmdline" || { echo 'Supervisor identity mismatch'; exit 1; }
        kill -TERM "$pid"
        count=0
        while kill -0 "$pid" 2>/dev/null; do
            grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
            count=$((count+1)); [ "$count" -lt 10 ] || exit 1; sleep 1
        done
    fi;;
esac
stop_owned "$root/ui.pid" "$root/hr54-ui"
stop_owned "$backend/jf.pid" "$backend/bin/hr54-jf"
cp "$backup/hr54-jf" "$backend/bin/hr54-jf.next"
chmod 700 "$backend/bin/hr54-jf.next"
mv "$backend/bin/hr54-jf.next" "$backend/bin/hr54-jf"
rm -f "$backend/doom/ENABLED" "$backend/doom/DISABLED"
for flag in ENABLED DISABLED; do
    [ ! -f "$backup/doom-$flag" ] || cp "$backup/doom-$flag" "$backend/doom/$flag"
done
for name in hr54-doom-native; do
    rm -f "$backend/doom/bin/$name"
    [ ! -f "$backup/$name" ] || cp "$backup/$name" "$backend/doom/bin/$name"
done
for name in doom1.wad README.TXT SOURCE.md; do
    rm -f "$backend/doom/data/$name"
    [ ! -f "$backup/$name" ] || cp "$backup/$name" "$backend/doom/data/$name"
done
# The pause flag was created for the native validation session, not normal use.
rm -f "$backend/ui/hijack-pause"
umount /opt/dtv/dtv.car
dt stop > "$root/rollback-middleware.log" 2>&1
(trap '' HUP; exec dt run) >> "$root/rollback-middleware.log" 2>&1 < /dev/null &
(trap '' HUP; exec "$backend/bin/hr54-jf" "$backend/www" 192.168.88.38 8096 8130) > "$root/rollback-backend.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$backend/jf.pid"
sync
echo 'Legacy backend and original stock MENU policy restored'
