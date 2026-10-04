#!/bin/sh
# Broker failure loses vendor ownership; restart promptly and log the outage.
set -eu
root=/var/hr54-persist/native-menu
lock=/tmp/hr54-input-supervisor.lock
log=$root/input-broker.log
child=
if [ -f "$root/INPUT_TRACE" ]; then export HR54_INPUT_TRACE=1; fi
mkdir "$lock" 2>/dev/null || exit 0
cleanup(){ if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || :; wait "$child" 2>/dev/null || :; fi; rm -f "$lock/pid" "$root/input-broker.pid"; rmdir "$lock" 2>/dev/null || :; }
trap cleanup EXIT
trap 'exit 0' INT TERM
printf '%s\n' "$$" > "$lock/pid"
while [ -f "$root/ENABLED" ] && [ -f "$root/BROKER_ENABLED" ]; do
    printf '%s broker starting; hardware ownership is unavailable until registration succeeds\n' "$(date '+%Y-%m-%dT%H:%M:%S')" >> "$log"
    "$root/hr54-input-broker" >> "$log" 2>&1 &
    child=$!
    printf '%s\n' "$child" > "$root/input-broker.pid"
    status=0
    wait "$child" || status=$?
    child=
    rm -f "$root/input-broker.pid"
    printf '%s broker exited status=%s; retrying\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "${status:-0}" >> "$log"
    sleep 2
done
