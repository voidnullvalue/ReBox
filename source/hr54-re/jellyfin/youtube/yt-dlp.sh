#!/bin/sh
export PYTHONHOME=/var/hr54-persist/jellyfin/youtube/python
export SSL_CERT_FILE=/var/hr54-persist/jellyfin/iptv/ca-certificates.crt
exec /var/hr54-persist/jellyfin/youtube/bin/python /var/hr54-persist/jellyfin/youtube/yt-dlp.zip --config-locations /var/hr54-persist/jellyfin/youtube/yt-dlp.conf "$@"
