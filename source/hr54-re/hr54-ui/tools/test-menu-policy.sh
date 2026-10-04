#!/bin/sh
# Temporary receiver-side test. Invoke explicitly; never installed as a boot
# hook. The native UI has no expiry. A reboot removes the RAM bind mount.
set -eu
ui_root=/tmp/hr54-native-test
policy_root=/var/hr54-persist/native-menu-test
policy_file=$policy_root/dtv-menu-policy.car
stock_file=/opt/dtv/dtv.car
record() { printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*"; }
stock_md5=$(md5sum "$stock_file")
case "$stock_md5" in 882124071cfe3ac1740af18161995dcf\ *) ;; *) record 'stock map mismatch; aborting'; exit 1;; esac
policy_md5=$(md5sum /var/hr54-transfer/dtv-menu-policy.car)
case "$policy_md5" in 2f62d0182a2c043affa69c966f240734\ *) ;; *) record 'staged map mismatch; aborting'; exit 1;; esac
wget -qO- http://127.0.0.1:8132/api/system/status > "$ui_root/menu-readiness.json"
grep -q '"doomRunning":false' "$ui_root/menu-readiness.json"
grep -q '"frontend":"native"' "$ui_root/menu-readiness.json"
mkdir -p "$policy_root"
chmod 700 "$policy_root"
mv /var/hr54-transfer/dtv-menu-policy.car "$policy_file"
chmod 600 "$policy_file"
ui_old=$(cat "$ui_root/ui.pid")
case "$ui_old" in ''|*[!0-9]*) exit 1;; esac
[ "$(readlink "/proc/$ui_old/exe")" = "$ui_root/hr54-ui" ]
record 'releasing native surface and input for requested MENU repair'
kill -TERM "$ui_old"
ui_wait=0
while kill -0 "$ui_old" 2>/dev/null; do
    ui_wait=$((ui_wait+1))
    [ "$ui_wait" -lt 8 ] || { record 'native cleanup incomplete; aborting'; exit 1; }
    sleep 1
done
cp "$ui_root/ui.log" "$ui_root/ui-before-menu.log"
mount --bind "$policy_file" "$stock_file"
record 'temporary MENU map mounted; reloading middleware using stock stop/run'
# The stock stop path stops its watchdog together with the media processes.
# Avoid killing Siege in isolation, which would trigger watchdog recovery.
dt stop
(trap '' HUP; exec dt run) > "$ui_root/menu-middleware.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$ui_root/menu-middleware.pid"
ui_wait=0
while ! pidof dtvwm >/dev/null || ! pidof keydispatcher >/dev/null; do
    ui_wait=$((ui_wait+1))
    [ "$ui_wait" -lt 90 ] || { record 'middleware startup failed; reboot restores stock map'; exit 1; }
    sleep 1
done
sleep 1
if "$ui_root/hr54-ui" --probe-input > "$ui_root/menu-probe.log" 2>&1; then
    record 'MENU ownership probe accepted; starting unlimited native test'
    (trap '' HUP; exec /tmp/hr54-native-test/hr54-ui --port 8132 --seconds 0) > "$ui_root/ui.log" 2>&1 < /dev/null &
else
    record 'MENU probe failed; restoring unlimited manual test for diagnosis'
    (trap '' HUP; exec /tmp/hr54-native-test/hr54-ui --manual --port 8132 --seconds 0) > "$ui_root/ui.log" 2>&1 < /dev/null &
fi
printf '%s\n' "$!" > "$ui_root/ui.pid"
record 'test remains active until user acknowledgement; no automatic cleanup'
