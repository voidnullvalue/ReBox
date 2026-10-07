#!/bin/sh
# Compatibility entry for the existing read-only asset-7 boot hook.
# That hook retains its firmware, root/firewall and stock-indexer behavior.
exec /var/hr54-persist/rebox-start.sh
