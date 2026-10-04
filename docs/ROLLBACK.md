# Rollback and recovery

Never delete existing snapshots. The authoritative pre-install recovery is YOUR
original same-receiver `/var` partition backup and saved geometry.
Offline preparation keeps a new `rebox-stock-backup.*` directory; activation
backs up CAR/registry/config under
`/var/hr54-persist/native-menu/backup/rebox-stock-activation.*` and records its
path in `native-menu/rebox-rollback-path`.

## Disable native presentation, keep root access and accounts

Stop media/Doom through the API first. When API idle checks pass:

```sh
HR54_DRAIN=5 tools/recv/hr54-shell.sh '/var/hr54-persist/deactivate-native.sh RESTORE_STOCK_PRESENTATION'
HR54_DRAIN=3 tools/recv/hr54-shell.sh 'sync; reboot'
```

The helper moves native activation flags into a fresh backup and restores the
exact saved stock Druid Auto-Start. Reboot removes RAM-only CAR mounts and
cleans up native processes. It keeps root hook, payload, accounts and all backups.
Check stock firmware CAR hash and Druid value after boot. Do not use the old
native-menu rollback source: it assumes an older pre-native backend and does not
restore the new suppression/broker configuration correctly.

If API or helper checks fail, do NOT weaken firmware/PID guards or kill the
middleware piecemeal. Use offline recovery.

## Complete original-stock recovery

Power off/remove the HDD. Resolve YOUR disk serial and all geometry again,
ensure all partitions are unmounted, verify original backup SHA-256, and restore
that SAME receiver's original partition-2 image using the deliberate procedure
in STOCK-INSTALL.md. Compare bytes and set the disk read-only before disconnect.
Do not restore a different receiver's image or write partitions 1/3/4/flash.
Restoring the original partition backup rolls back post-backup `/var` state,
including any new configuration; do not apply it after weeks of normal use
without reviewing changes/new recording metadata. Keep current backups too.

This bundle does not perform an automatic full-disk erase, factory reset or
delete recordings. Failure of the custom GUI is not a reason to destroy data.
