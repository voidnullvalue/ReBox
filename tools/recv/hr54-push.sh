#!/usr/bin/env bash
# push.sh <local-file> <remote-path>   base64 + busybox uudecode over the HR54 root shell
set -uo pipefail
LOCAL="$1"; REMOTE="$2"
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$(mktemp)
{
  printf 'rm -f %s.b64 %s\n' "$REMOTE" "$REMOTE"
  printf "cat >> %s.b64 <<'HR54B64EOF'\n" "$REMOTE"
  python3 -c '
import sys,base64
b=base64.b64encode(open(sys.argv[1],"rb").read()).decode()
for i in range(0,len(b),200): print(b[i:i+200])
' "$LOCAL"
  printf 'HR54B64EOF\n'
  printf '{ echo "begin-base64 644 %s"; cat %s.b64; echo "===="; } > %s.uu\n' "$REMOTE" "$REMOTE" "$REMOTE"
  printf 'busybox uudecode -o %s %s.uu; echo "uudecode rc=$?"\n' "$REMOTE" "$REMOTE"
  printf 'rm -f %s.b64 %s.uu\n' "$REMOTE" "$REMOTE"
  printf 'md5sum %s\n' "$REMOTE"
  printf 'ls -la %s\n' "$REMOTE"
} > "$SCRIPT"
"$HERE/hr54-shell.sh" -f "$SCRIPT" 2>/dev/null | grep -v '^hr54-root' | grep -v '^> '
rm -f "$SCRIPT"
