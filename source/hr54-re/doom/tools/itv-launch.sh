#!/bin/sh
# Launch a URL in the receiver's ITV/WebKit renderer through the same route
# jellyfin/remote/hr54_jf.c rc_present_tv_ui() uses.
#
#   tools/itv-launch.sh <url>            launch (stops any running app first)
#   tools/itv-launch.sh                  launch $DOOM_URL
#   tools/itv-launch.sh <url> --keep     launch only if nothing is running
#   tools/itv-launch.sh --status         just report
#
# ItvStopApp is required first: itvStartApp answers "Another app is running"
# and changes nothing if an app is already live.
set -u
U="/opt/middleware_core/system/tv/uconntest"
URL="${1:-}"
if [ -z "$URL" ] && [ -f /var/hr54-persist/doom.url ]; then
    URL=$(cat /var/hr54-persist/doom.url)
fi
case "$URL" in
  --status|"")
    $U '<com.ucentric.pvruconnect.DirectTest command="itvGetAppStatus" sessionId="0"/>' 2>&1
    exit 0
    ;;
esac
if [ "${2:-}" = "--keep" ]; then
    $U '<com.ucentric.pvruconnect.DirectTest command="itvGetAppStatus" sessionId="0"/>' 2>&1 |
        grep -q "Running appId" && { echo "itv already running"; exit 0; }
fi
# Druid must be on LiveTV or the ITV plane is covered by the menu, and a boot
# OSD left over from power-on covers everything.
OSD=/var/mw_registry/Registry/Device/Server/OSD/Current
if [ "$(cat $OSD/Number 2>/dev/null)" = '"36"' ]; then
    /usr/bin/dt removeOsd -osd 36 -session 0 >/dev/null 2>&1
fi
$U '<com.ucentric.pvruconnect.DirectTest command="itvStopApp" sessionId="0"/>' >/dev/null 2>&1
sleep 3
# itvStartApp answers "Another app is running" while the previous app is still
# being torn down, and it does not queue.  Retry rather than lose the launch.
TRIES=0
while [ $TRIES -lt 4 ]; do
    OUT=$($U "<com.ucentric.pvruconnect.DirectTest command=\"itvStartApp\" sessionId=\"0\" url=\"$URL\"/>" 2>&1)
    case "$OUT" in
        *"Another app is running"*) sleep 4 ;;
        *) echo "$OUT"; break ;;
    esac
    TRIES=$((TRIES + 1))
done
sleep 5
$U '<com.ucentric.pvruconnect.DirectTest command="itvGetAppStatus" sessionId="0"/>' 2>&1
