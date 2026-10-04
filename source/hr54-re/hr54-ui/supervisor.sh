#!/bin/sh
# Explicit native-menu activation, launched after the boot policy is loaded.
# ACTIVATED records deployment, not completion of the entire acceptance matrix.
set -eu
ui_start=${1:-}
case "$ui_start" in ''|--start-hidden) :;; *) exit 2;; esac
ui_root=/var/hr54-persist/native-menu
ui_lock=/tmp/hr54-ui-supervisor.lock
ui_child=
ui_enabled=$ui_root/ENABLED
ui_log=$ui_root/startup.log
if [ -f "$ui_root/BROKER_BYPASSED" ]; then export HR54_DIRECT_INPUT=1; fi
if [ -f "$ui_root/INPUT_TRACE" ]; then export HR54_INPUT_TRACE=1; fi
[ -f "$ui_enabled" ] || exit 0
[ -f "$ui_root/ACTIVATED" ] || exit 0
[ -x "$ui_root/hr54-ui" ] || exit 0
if ! mkdir "$ui_lock" 2>/dev/null; then
    # A stale lock is reclaimed only when its recorded process is absent.
    ui_old=$(cat "$ui_lock/pid" 2>/dev/null || :)
    case "$ui_old" in ''|*[!0-9]*) exit 1;; esac
    [ ! -d "/proc/$ui_old" ] || exit 0
    rm -f "$ui_lock/pid"
    rmdir "$ui_lock" || exit 1
    mkdir "$ui_lock" || exit 1
fi
printf '%s\n' "$$" > "$ui_lock/pid"
now() { read ui_uptime ui_unused < /proc/uptime; printf '%s\n' "${ui_uptime%%.*}"; }
record() { printf '%s uptime=%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$(now)" "$*" >> "$ui_log"; }
cleanup() {
    if [ -n "$ui_child" ]; then kill -TERM "$ui_child" 2>/dev/null || :; wait "$ui_child" 2>/dev/null || :; fi
    rm -f "$ui_lock/pid" "$ui_root/ui.pid"
    rmdir "$ui_lock" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 0' INT TERM
ui_wait_seconds=${HR54_BOOT_WAIT_SECONDS:-120}
case "$ui_wait_seconds" in ''|*[!0-9]*) exit 1;; esac
[ "$ui_wait_seconds" -gt 0 ] && [ "$ui_wait_seconds" -le 300 ] || exit 1
ui_deadline=$(($(now)+ui_wait_seconds))
while [ -f "$ui_enabled" ]; do
    if pidof dtvwm >/dev/null && pidof keydispatcher >/dev/null &&
       "$ui_root/hr54-ui" --check-api; then break; fi
    if [ "$(now)" -ge "$ui_deadline" ]; then record 'services not ready; startup deadline expired'; exit 1; fi
    sleep 2
done
[ -f "$ui_enabled" ] || exit 0
ui_attempt=0
while [ -f "$ui_enabled" ]; do
    if [ "$ui_attempt" -ge 3 ]; then
        if [ ! -f "$ui_root/BROKER_ENABLED" ]; then break; fi
        record 'frontend restart batch exhausted; input broker retained, retrying in 15s'
        sleep 15
        ui_attempt=0
    fi
    ui_attempt=$((ui_attempt+1))
    record "starting UI attempt=$ui_attempt"
    if [ "$ui_start" = --start-hidden ]; then
        "$ui_root/hr54-ui" --start-hidden >> "$ui_log" 2>&1 </dev/null &
    else
        "$ui_root/hr54-ui" >> "$ui_log" 2>&1 </dev/null &
    fi
    ui_child=$!
    printf '%s\n' "$ui_child" > "$ui_root/ui.pid"
    ui_exit=0
    wait "$ui_child" || ui_exit=$?
    ui_child=
    rm -f "$ui_root/ui.pid"
    record "UI exited status=$ui_exit"
    # A clean frontend exit still needs a replacement while enabled. Signals
    # sent to this supervisor use its trap and leave the loop intentionally.
    if [ "$ui_exit" -eq 0 ]; then ui_attempt=0; fi
    sleep 2
done
record 'restart budget exhausted; explicit recovery remains available'
