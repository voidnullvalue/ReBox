#!/bin/sh
# Generic persistent launcher. Optional module configuration never gates core boot.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
umask 077
base=/var/hr54-persist
runtime=$base/rebox
menu=$base/native-menu
[ -f "$base/REBOX_ENABLED" ] || exit 0
[ -x "$runtime/bin/reboxd" ] && [ ! -L "$runtime/bin/reboxd" ] || exit 1
mkdir -p "$runtime/log" /var/opt/hr54/bin
cp "$base/bin/hr54-play-url" /var/opt/hr54/bin/hr54-play-url
chmod 700 /var/opt/hr54/bin/hr54-play-url
pid=$(cat "$runtime/reboxd.pid" 2>/dev/null || :)
running=false
case "$pid" in ''|*[!0-9]*) :;; *)
    if [ "$pid" -gt 1 ] && [ -d "/proc/$pid" ] && [ "$(readlink "/proc/$pid/exe")" = "$runtime/bin/reboxd" ]; then running=true; fi;;
esac
if [ "$running" = false ]; then
    (trap '' HUP; exec "$runtime/bin/reboxd" "$runtime" 8130 /tmp/rebox-modules) >> "$runtime/log/reboxd.log" 2>&1 < /dev/null &
    printf '%s\n' "$!" > "$runtime/reboxd.pid"
fi
# Reuse the existing, firmware-checked policy/bootstrap and persistent broker.
# On a warm handover its loaded-boot guard rejoins without a middleware reload.
if [ -f "$menu/ENABLED" ] && [ -f "$menu/ACTIVATED" ]; then
    (trap '' HUP; exec "$menu/bootstrap.sh") >> "$menu/bootstrap-runtime-launch.log" 2>&1 < /dev/null &
    printf '%s\n' "$!" > "$menu/bootstrap-launch.pid"
fi
