#!/usr/bin/env bash
# Wait for receiver services; input/HDMI acceptance is a separate check.
set -uo pipefail
host=${HR54_HOST:?Set HR54_HOST to your receiver IPv4}
here=$(cd -- "$(dirname -- "$0")" && pwd)
for i in $(seq 1 "${1:-24}"); do
  A=$(timeout 2 bash -c 'exec 3<>"/dev/tcp/$1/5777"' bash "$host" 2>/dev/null && echo Y || echo n)
  B=$(timeout 2 bash -c 'exec 3<>"/dev/tcp/$1/8130"' bash "$host" 2>/dev/null && echo Y || echo n)
  U=
  if [ "$A" = Y ]; then
    U=$(HR54_TIMEOUT=5 HR54_DRAIN=1 "$here/hr54-shell.sh" 'cut -d" " -f1 /proc/uptime' 2>/dev/null | grep -oE '^[0-9]+\.?[0-9]*' | head -1)
  fi
  echo "$(date +%T)  5777=$A 8130=$B uptime=${U:-?}"
  if [ "$A" = "Y" ] && [ "$B" = "Y" ]; then echo "BOTH SERVICES UP"; exit 0; fi
  sleep 20
done
echo "TIMED OUT"
exit 1
