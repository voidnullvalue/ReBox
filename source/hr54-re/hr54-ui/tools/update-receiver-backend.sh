#!/bin/sh
# Preserve the installed launcher, configuration and middleware during repair.
set -eu
root=/var/hr54-persist/native-menu
backend=/var/hr54-persist/jellyfin
expected=${1:?expected MD5 required}
staged=${2:?staged backend path required}
case "$expected" in *[!0-9a-f]*|'') exit 2;; esac
[ "${#expected}" -eq 32 ]
case "$staged" in /var/hr54-transfer/*) :;; *) exit 2;; esac
[ "$(md5sum "$staged" | cut -d' ' -f1)" = "$expected" ]
"$root/hr54-ui" --check-api --idle
pid=$(cat "$backend/jf.pid")
case "$pid" in ''|*[!0-9]*) exit 1;; esac
[ "$(readlink "/proc/$pid/exe")" = "$backend/bin/hr54-jf" ]
[ -f "$root/backup/hr54-jf-before-navigation-repair" ] ||
    cp "$backend/bin/hr54-jf" "$root/backup/hr54-jf-before-navigation-repair"
cp "$staged" "$backend/bin/hr54-jf.next"
chmod 700 "$backend/bin/hr54-jf.next"
kill -TERM "$pid"
count=0
while kill -0 "$pid" 2>/dev/null; do
    grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
    count=$((count+1)); [ "$count" -lt 12 ] || { echo 'Backend shutdown incomplete; old binary retained'; exit 1; }
    sleep 1
done
mv "$backend/bin/hr54-jf.next" "$backend/bin/hr54-jf"
(trap '' HUP; exec "$backend/bin/hr54-jf" "$backend/www" 192.168.88.38 8096 8130) >> "$root/backend.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$backend/jf.pid"
sync
echo 'Backend updated through the existing persistent path'
