#!/bin/sh
set -eu
umask 077
base=/var/hr54-persist
menu=$base/native-menu
work=$(mktemp -d "$base/backup/provider-repair-20261007.XXXXXX")
mkdir -p "$work/previous" "$work/original"
cp -p "$base/jellyfin/bin/hr54-jf" "$work/previous/hr54-jf"
cp -p "$base/rebox-start.sh" "$work/previous/rebox-start.sh"
cp -p "$menu/hr54-ui" "$work/previous/hr54-ui"
(cd "$work/original"; tar -xf "$base/backup/restore-from-ReBox-20261007/before.tar" rebox-start.sh jellyfin/bin/hr54-jf)
[ "$(md5sum "$work/original/jellyfin/bin/hr54-jf" | cut -d' ' -f1)" = 804613d0f5b41984d0b65f38397f3780 ]
[ "$(md5sum "$base/rebox/bin/reboxd" | cut -d' ' -f1)" = 0ae19ef10716fd04cad1569ec1463835 ]
[ "$(md5sum /var/hr54-transfer/rebox-prepare-ui-20261008 | cut -d' ' -f1)" = 52868f7a8d34fbe5b1b23bd5185c4ddd ]
boot=$(cat /proc/sys/kernel/random/boot_id)
policy=$(cat "$menu/policy.md5")
case "$(cat "$menu/loaded-boot")" in "$boot:$policy:"*:native) :;; *) exit 1;; esac
[ "$(md5sum /opt/dtv/dtv.car | cut -d' ' -f1)" = "$policy" ]
"$menu/hr54-ui" --check-api --idle
ui=$(cat "$menu/ui.pid")
supervisor=$(cat /tmp/hr54-ui-supervisor.lock/pid)
backend=$(cat "$base/jellyfin/jf.pid")
case "$ui:$supervisor:$backend" in *[!0-9:]*|:*) exit 1;; esac
[ "$(readlink /proc/$ui/exe)" = "$menu/hr54-ui" ]
[ "$(readlink /proc/$backend/exe)" = "$base/jellyfin/bin/hr54-jf" ]
grep -q "$menu/supervisor.sh" /proc/$supervisor/cmdline
kill -TERM "$supervisor"
for pid in "$supervisor" "$ui"; do
 n=0
 while kill -0 "$pid" 2>/dev/null; do
  grep -q '^State:.*Z' /proc/$pid/status 2>/dev/null && break
  n=$((n+1)); [ "$n" -lt 15 ] || exit 1; sleep 1
 done
done
kill -TERM "$backend"
n=0
while kill -0 "$backend" 2>/dev/null; do
 grep -q '^State:.*Z' /proc/$backend/status 2>/dev/null && break
 n=$((n+1)); [ "$n" -lt 15 ] || exit 1; sleep 1
done
cp -p "$work/original/rebox-start.sh" "$base/rebox-start.sh.next"
chmod 700 "$base/rebox-start.sh.next"
mv "$base/rebox-start.sh.next" "$base/rebox-start.sh"
cp -p "$work/original/jellyfin/bin/hr54-jf" "$base/jellyfin/bin/hr54-jf.next"
chmod 700 "$base/jellyfin/bin/hr54-jf.next"
mv "$base/jellyfin/bin/hr54-jf.next" "$base/jellyfin/bin/hr54-jf"
cp /var/hr54-transfer/rebox-prepare-ui-20261008 "$menu/hr54-ui.next"
chmod 700 "$menu/hr54-ui.next"
mv "$menu/hr54-ui.next" "$menu/hr54-ui"
printf '%s\n' "$work" > "$base/provider-repair-path"
"$base/rebox-start.sh"
sync
printf 'PROVIDER_RUNTIME_REJOIN_STARTED backup=%s\n' "$work"
