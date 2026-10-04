#!/bin/sh
# Explicit deployment to the existing persistent userland launcher. No flash.
set -eu
root=/var/hr54-persist/native-menu
backend=/var/hr54-persist/jellyfin
stage=/var/hr54-transfer
backup=$root/backup
digest() { md5sum "$1" | cut -d' ' -f1; }
stop_owned() {
    file=$1 expected=$2
    pid=$(cat "$file" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) return 0;; esac
    [ -d "/proc/$pid" ] || return 0
    [ "$(readlink "/proc/$pid/exe")" = "$expected" ] || { echo 'PID identity mismatch; aborting'; exit 1; }
    kill -TERM "$pid"
    count=0
    while kill -0 "$pid" 2>/dev/null; do
        # An adopted zombie has already released its sockets and surface.
        grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null && break
        count=$((count+1)); [ "$count" -lt 10 ] || { echo 'Clean shutdown incomplete'; exit 1; }
        sleep 1
    done
}
[ ! -e "$root/ACTIVATED" ] || { echo 'Already activated; use the update procedure'; exit 1; }
cd "$stage"
md5sum -c native-menu-staged.md5
[ "$(digest /opt/dtv/dtv.car)" = 2f62d0182a2c043affa69c966f240734 ]
[ "$(digest /var/hr54-persist/native-menu-test/dtv-menu-policy.car)" = 2f62d0182a2c043affa69c966f240734 ]
[ "$(digest /tmp/hr54-native-test/doom/data/doom1.wad)" = f0cefca49926d00903cf57551d901abe ]
mkdir -p "$root" "$backup" "$backend/doom/bin" "$backend/doom/data"
chmod 700 "$root" "$backup"
[ ! -e "$backup/hr54-jf" ] || { echo 'Existing backup; refusing to overwrite'; exit 1; }
cp "$backend/bin/hr54-jf" "$backup/hr54-jf"
for flag in ENABLED DISABLED; do
    if [ -f "$backend/doom/$flag" ]; then cp "$backend/doom/$flag" "$backup/doom-$flag"; fi
done
[ ! -f "$backend/ui/hijack-pause" ] || : > "$backup/hijack-pause-was-present"
for name in hr54-doom-native; do
    [ ! -f "$backend/doom/bin/$name" ] || cp "$backend/doom/bin/$name" "$backup/$name"
done
for name in doom1.wad README.TXT SOURCE.md; do
    [ ! -f "$backend/doom/data/$name" ] || cp "$backend/doom/data/$name" "$backup/$name"
done
cp native-menu-ui.next "$root/hr54-ui"
cp native-menu-bootstrap.next "$root/bootstrap.sh"
cp native-menu-supervisor.next "$root/supervisor.sh"
cp native-menu-rollback.next "$root/rollback.sh"
cp native-menu-OFL.txt "$root/OFL.txt"
cp native-menu-doom-LICENSE.txt "$backend/doom/LICENSE.txt"
cp /var/hr54-persist/native-menu-test/dtv-menu-policy.car "$root/dtv-menu-policy.car"
cp /tmp/hr54-native-test/doom/bin/hr54-doom-native "$backend/doom/bin/hr54-doom-native"
cp /tmp/hr54-native-test/doom/data/doom1.wad "$backend/doom/data/doom1.wad"
cp /tmp/hr54-native-test/doom/data/README.TXT "$backend/doom/data/README.TXT"
cp native-menu-doom-SOURCE.md "$backend/doom/data/SOURCE.md"
chmod 700 "$root/hr54-ui" "$root/bootstrap.sh" "$root/supervisor.sh" "$root/rollback.sh" "$backend/doom/bin/hr54-doom-native"
chmod 600 "$root/dtv-menu-policy.car" "$root/OFL.txt" "$backend/doom/data/doom1.wad" "$backend/doom/data/README.TXT" "$backend/doom/data/SOURCE.md"
# The staged backend/API runs under the temporary root first. Its idle check
# must pass before any live parent is stopped; no receiver media calls here.
"$root/hr54-ui" --port 8132 --check-api --idle
stop_owned /tmp/hr54-native-test/ui.pid /tmp/hr54-native-test/hr54-ui
stop_owned /tmp/hr54-native-test/backend.pid /tmp/hr54-native-test/hr54-jf
stop_owned "$backend/jf.pid" "$backend/bin/hr54-jf"
cp native-menu-backend.next "$backend/bin/hr54-jf.next"
chmod 700 "$backend/bin/hr54-jf.next"
mv "$backend/bin/hr54-jf.next" "$backend/bin/hr54-jf"
rm -f "$backend/doom/DISABLED"
: > "$backend/doom/ENABLED"
# This boot's MENU policy was already loaded and physically acknowledged.
# Do not perform a second middleware reload during promotion.
strings=stock
if [ -f /var/hr54-persist/overlays/strings-boot.enabled ]; then
    strings=$(digest /var/hr54-persist/overlays/englishtext-755.txt)
    [ "$(digest /opt/ui_assets/assetspack/language/englishtext.txt)" = "$strings" ]
fi
printf '%s:2f62d0182a2c043affa69c966f240734:%s\n' "$(cat /proc/sys/kernel/random/boot_id)" "$strings" > "$root/loaded-boot"
printf '%s\n' 'User requested native MENU deployment; full acceptance matrix remains open.' > "$root/ACTIVATED"
: > "$root/ENABLED"
chmod 600 "$root/ACTIVATED" "$root/ENABLED" "$root/loaded-boot" "$backend/doom/ENABLED"
(trap '' HUP; exec "$backend/bin/hr54-jf" "$backend/www" 192.168.88.38 8096 8130) > "$root/backend.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$backend/jf.pid"
sync
echo 'Native menu activated through the existing persistent backend launcher'
