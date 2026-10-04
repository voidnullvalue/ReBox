#!/usr/bin/env bash
set -uo pipefail
HOST="${HR54_HOST:-192.168.88.103}"
PORT="${HR54_PORT:-5777}"
SCRIPT=$(mktemp)
if [ "${1:-}" = "-f" ]; then cat "${2:?}" > "$SCRIPT"; else printf '%s\n' "${1:-}" > "$SCRIPT"; fi
# HR54_DRAIN is the pause before nc closes the write side. The default of 1s is
# fine for short command output but truncates large output (a hexdump of a
# 100KB binary), because nc -q 2 gives up before the box finishes writing.
out=$( { cat "$SCRIPT"; printf '\nexit\n'; sleep "${HR54_DRAIN:-1}"; } | timeout "${HR54_TIMEOUT:-60}" nc -q 2 "$HOST" "$PORT" 2>&1 )
rm -f "$SCRIPT"
printf '%s\n' "$out" | sed 's/^hr54-root# //'
