#!/usr/bin/env sh
# Test stand-in for /var/opt/hr54/bin/hr54-play-url: launches the decoder
# fetch in the background and returns success immediately, like the receiver
# wrapper does with DirectTest playURL.
exec >/dev/null 2>&1
DIR=$(dirname "$0")
python3 "$DIR/mock_fetch.py" "$1" &
exit 0
