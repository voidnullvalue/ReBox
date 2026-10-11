#!/bin/sh
# Jeopardy module-owned persistent LAN firewall boot hook.
#
# Marker: JEOPARDY-LAN-BOOT-HOOK
#
# Owns exactly one INPUT ACCEPT rule for the module's LAN service. The scope is
# fixed and minimal: the wired LAN interface (default eth0), the private LAN
# subnet (default 192.168.88.0/24) and the module's own TCP port (default 8135).
# It never widens LAN/WAN reachability, never touches any other INPUT rule and
# never edits the base/stock firewall policy or persistence.
#
# It is additive and fail-closed. The owned rule exists only while the ReBox
# root marker, the Jeopardy module manifest and its schema-1 module state all
# agree the module is installed and enabled. In every other case (marker
# missing, module absent, disabled, uninstalled or malformed state) the single
# owned rule is removed and nothing else is changed.
#
# The operation is idempotent: repeated runs never accumulate duplicates.
# iptables 1.4.7 has no -C, so presence is matched against the canonical
# `iptables -S INPUT` listing (trailing whitespace stripped) and only the exact
# owned rule spec is added or removed. It is deployed outside the module package
# by tools/install-persistence.py; it is never installed by the module runtime.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
umask 077

base=${HR54_PERSIST_BASE:-/var/hr54-persist}
runtime=$base/rebox
state=$runtime/module-state/jeopardy.json
manifest=$runtime/modules/jeopardy/module.json
conf=$runtime/boot.d/jeopardy-network.conf
iptables=${IPTABLES:-iptables}

iface=eth0
lan=192.168.88.0/24
port=8135

# Read the private, root-owned config (regular file only). Every field is
# whitelisted; anything unexpected is ignored and the safe default retained.
if [ -f "$conf" ] && [ ! -L "$conf" ]; then
    v=$(sed -n 's/^INTERFACE=//p' "$conf" 2>/dev/null | head -n 1)
    case "$v" in
        ''|*[!A-Za-z0-9_:.-]*) : ;;
        *) iface=$v ;;
    esac
    v=$(sed -n 's/^LAN=//p' "$conf" 2>/dev/null | head -n 1)
    case "$v" in
        *[!0-9./]*|'') : ;;
        */*)
            p=${v#*/}
            case "$p" in
                ''|*[!0-9]*) : ;;
                0) : ;;
                *) lan=$v ;;
            esac ;;
        *) : ;;
    esac
    v=$(sed -n 's/^PORT=//p' "$conf" 2>/dev/null | head -n 1)
    case "$v" in
        ''|*[!0-9]*) : ;;
        *) [ "$v" -ge 1 ] && [ "$v" -le 65535 ] && port=$v ;;
    esac
fi

# Canonical `iptables -S` form: -s precedes -i and an implicit `-m tcp` is shown.
rule_a="-A INPUT -s $lan -i $iface -p tcp -m tcp --dport $port -j ACCEPT"
rule_b="-A INPUT -s $lan -i $iface -p tcp --dport $port -j ACCEPT"
spec="-i $iface -s $lan -p tcp --dport $port -j ACCEPT"

# busybox 1.16.1 grep has no -x, so match exact rule lines with a bounded
# pure-shell read that drops the leading/trailing whitespace `iptables -S`
# emits (default IFS trims it). No external text tools are required.
count_owned() {
    n=0
    while read -r line; do
        if [ "$line" = "$rule_a" ] || [ "$line" = "$rule_b" ]; then
            n=$((n + 1))
        fi
    done <<EOF
$("$iptables" -S INPUT 2>/dev/null || true)
EOF
    echo "$n"
}
present() { [ "$(count_owned)" -ge 1 ] 2>/dev/null; }

remove_owned() {
    n=0
    while [ "$n" -lt 16 ] && present; do
        "$iptables" -D INPUT $spec >/dev/null 2>&1 || break
        n=$((n + 1))
    done
}

module_active() {
    [ -f "$base/REBOX_ENABLED" ] || return 1
    [ -f "$manifest" ] && [ ! -L "$manifest" ] || return 1
    [ -f "$state" ] && [ ! -L "$state" ] || return 1
    grep -Eq '^\{"schema":1,"enabled":true,"installed":true,"version":"[^"]*"\}$' "$state"
}

# iptables is required to own any rule. Without it we change nothing.
command -v "$iptables" >/dev/null 2>&1 || {
    echo "jeopardy-network: iptables unavailable; no change"
    exit 0
}

if module_active; then
    n=$(count_owned)
    [ -n "$n" ] || n=0
    i=0
    while [ "$n" -gt 1 ] && [ "$i" -lt 16 ]; do
        "$iptables" -D INPUT $spec >/dev/null 2>&1 || break
        i=$((i + 1))
        n=$(count_owned)
        [ -n "$n" ] || n=0
    done
    if [ "$n" -ge 1 ]; then
        echo "jeopardy-network: owned LAN rule present"
    elif "$iptables" -I INPUT 1 $spec >/dev/null 2>&1; then
        echo "jeopardy-network: owned LAN rule added"
    else
        echo "jeopardy-network: failed to add owned LAN rule" >&2
    fi
else
    remove_owned
    echo "jeopardy-network: module inactive; owned LAN rule removed"
fi
