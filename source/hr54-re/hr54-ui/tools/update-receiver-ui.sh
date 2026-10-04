#!/bin/sh
# Replace only the frontend, preserving the running backend and media lifecycle.
set -eu
root=/var/hr54-persist/native-menu
expected=${1:?expected MD5 required}
staged=${2:?staged UI path required}
start=${3:-}
case "$start" in ''|--start-hidden) :;; *) exit 2;; esac
case "$expected" in *[!0-9a-f]*|'') exit 2;; esac
[ "${#expected}" -eq 32 ]
case "$staged" in /var/hr54-transfer/*) :;; *) exit 2;; esac
[ "$(md5sum "$staged" | cut -d' ' -f1)" = "$expected" ]
"$root/hr54-ui" --check-api
ui_pid=$(cat "$root/ui.pid")
supervisor_pid=$(cat /tmp/hr54-ui-supervisor.lock/pid)
case "$ui_pid:$supervisor_pid" in *[!0-9:]*|:*) exit 1;; esac
[ "$(readlink "/proc/$ui_pid/exe")" = "$root/hr54-ui" ]
grep -q "$root/supervisor.sh" "/proc/$supervisor_pid/cmdline"
old=$(md5sum "$root/hr54-ui" | cut -d' ' -f1)
[ -f "$root/backup/hr54-ui-$old" ] || cp "$root/hr54-ui" "$root/backup/hr54-ui-$old"
kill -TERM "$supervisor_pid"
for pid in "$supervisor_pid" "$ui_pid"; do
    count=0
    while kill -0 "$pid" 2>/dev/null; do
        grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
        count=$((count+1)); [ "$count" -lt 12 ] || { echo 'Clean UI shutdown incomplete; retaining old binary'; exit 1; }
        sleep 1
    done
done
cp "$staged" "$root/hr54-ui.next"
chmod 700 "$root/hr54-ui.next"
mv "$root/hr54-ui.next" "$root/hr54-ui"
if [ "$start" = --start-hidden ]; then
    (trap '' HUP; exec "$root/supervisor.sh" --start-hidden) >> "$root/supervisor-launch.log" 2>&1 < /dev/null &
else
    (trap '' HUP; exec "$root/supervisor.sh") >> "$root/supervisor-launch.log" 2>&1 < /dev/null &
fi
printf '%s\n' "$!" > "$root/supervisor-launch.pid"
sync
echo 'Native frontend updated; backend and middleware left running'
