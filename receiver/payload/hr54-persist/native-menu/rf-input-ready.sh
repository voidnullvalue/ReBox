#!/bin/sh
# Restore the vendor RF input service independently of the Druid presentation.
set -eu
root=/var/hr54-persist/native-menu
rf=/opt/rf4ce/bin/rf4cerc
kd=/opt/key_dispatcher/bin/keydispatcher
[ "$(md5sum "$rf" | cut -d' ' -f1)" = 6d8f995c7a225eded45c359a326930e6 ]
[ "$(md5sum "$kd" | cut -d' ' -f1)" = 651023fc848062808e63833af356d706 ]
kp=$(pidof keydispatcher)
case "$kp" in ''|*[!0-9]*) echo 'RF input: missing or ambiguous keydispatcher' >&2; exit 1;; esac
connected(){
    for fd in /proc/$kp/fd/*; do
        [ "$(readlink "$fd")" != /dev/nds/rf4ce/zrcinput ] || return 0
    done
    return 1
}
if connected && pidof rf4cerc >/dev/null; then exit 0; fi
if ! pidof rf4cerc >/dev/null; then
    (trap '' HUP; exec "$rf") >> "$root/rf4cerc-repair.log" 2>&1 < /dev/null &
    sleep 1
fi
reply=$("$rf" EnableDevice 2>&1) || { printf '%s\n' "$reply" >&2; exit 1; }
case "$reply" in *'enableDevice: device enabled'*) :;; *) printf '%s\n' "$reply" >&2; exit 1;; esac
connected || { echo 'RF input: ZRC descriptor absent after enable' >&2; exit 1; }
echo 'RF input: vendor radio enabled; keydispatcher ZRC descriptor open (physical acceptance still required)'
