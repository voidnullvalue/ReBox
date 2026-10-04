#!/bin/sh
# Persistent API launcher. Stock presentation stays intact until activation.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
umask 077
base=/var/hr54-persist
config=$base/rebox.conf
backend=$base/jellyfin
[ -f "$base/REBOX_ENABLED" ] || exit 0
[ -f "$config" ] && [ ! -L "$config" ] || exit 1
host=$(sed -n 's/^JELLYFIN_IPV4=//p' "$config")
port=$(sed -n 's/^JELLYFIN_PORT=//p' "$config")
case "$host" in ''|*[!0-9.]*) echo 'Invalid Jellyfin IPv4' >&2; exit 1;; esac
oldifs=$IFS; IFS=.; set -- $host; IFS=$oldifs
[ "$#" -eq 4 ] || exit 1
for octet do
    case "$octet" in ''|*[!0-9]*) exit 1;; esac
    [ "$octet" -le 255 ] || exit 1
done
case "$port" in ''|*[!0-9]*) exit 1;; esac
[ "$port" -gt 0 ] && [ "$port" -le 65535 ] || exit 1
mkdir -p /var/opt/hr54/bin /var/opt/hr54/log "$backend/log"
cp "$base/bin/hr54-play-url" /var/opt/hr54/bin/hr54-play-url
chmod 700 /var/opt/hr54/bin/hr54-play-url
pid=$(cat "$backend/jf.pid" 2>/dev/null || :)
case "$pid" in ''|*[!0-9]*) :;; *)
    if [ -d "/proc/$pid" ] &&
       [ "$(readlink "/proc/$pid/exe")" = "$backend/bin/hr54-jf" ]; then exit 0; fi;;
esac
# --no-launcher avoids legacy SHEF/ITV takeover before native activation.
# The native marker pair automatically enables the repaired bootstrap.
(trap '' HUP; exec "$backend/bin/hr54-jf" "$backend/www" "$host" "$port" 8130 --no-launcher) >> "$backend/log/jf.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$backend/jf.pid"
