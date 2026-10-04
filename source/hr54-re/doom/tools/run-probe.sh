#!/bin/sh
# Start (or restart) the DOOM probe server on the workstation.
set -u
cd /home/void/vroomfondle/dvr/hr54-re/doom
pkill -f 'probe_server.py' 2>/dev/null
sleep 1
( setsid python3 tools/probe_server.py 8130 \
    >/tmp/opencode/doom-probe.log 2>&1 </dev/null & )
sleep 3
curl -s -o /dev/null -w 'probe_server http=%{http_code}\n' http://127.0.0.1:8130/doom/
