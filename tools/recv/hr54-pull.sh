#!/usr/bin/env bash
# Copy a file off the receiver. The box has no base64, no uuencode, no od —
# only hexdump and dd — so bytes come back as a `hexdump -C` text dump and are
# reassembled on the host.
# Refuses /var/viewer: recordings are read-only for this task.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
HOST="${HR54_HOST:-192.168.88.103}"
REMOTE="${1:?usage: hr54-pull.sh REMOTE_PATH [LOCAL_PATH]}"
LOCAL="${2:-}"
[ -n "$LOCAL" ] || LOCAL="./$(basename "$REMOTE")"

case "$REMOTE" in
  /var/viewer/*)
    echo "refusing: /var/viewer is read-only for this task" >&2
    exit 2
    ;;
esac

SH=$(mktemp)
printf "hexdump -v -C '%s'\n" "$REMOTE" > "$SH"
raw=$(HR54_TIMEOUT=${HR54_TIMEOUT:-600} HR54_DRAIN=${HR54_DRAIN:-20} \
      tools/recv/hr54-shell.sh -f "$SH" 2>&1)
rm -f "$SH"

printf '%s\n' "$raw" | sed 's/^hr54-root# //' | python3 -c '
import re, sys
out = bytearray()
for line in sys.stdin:
    m = re.match(r"^\s*([0-9a-f]{8})\s", line)
    if not m:
        continue
    # left of the ASCII gutter is "offset  bb bb ... bb |"
    left = line.split("|", 1)[0][m.end(1):]
    hexbytes = re.findall(r"\b[0-9a-f]{2}\b", left)
    out += bytes.fromhex("".join(hexbytes))
sys.stdout.buffer.write(bytes(out))
' > "$LOCAL"

if [ -s "$LOCAL" ]; then
  echo "OK $LOCAL ($(wc -c < "$LOCAL") bytes) from $REMOTE"
else
  echo "pull failed: $REMOTE" >&2
  exit 1
fi
