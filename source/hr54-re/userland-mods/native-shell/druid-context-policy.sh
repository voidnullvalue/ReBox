#!/bin/sh
# Supported stock UI context configuration only. Never reloads middleware.
set -eu
umask 077
registry=/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID
menu=/var/hr54-persist/native-menu
auto=$registry/Auto-Start.str
main=$registry/Context-Main.str
expected_main=com.directv.druid.DruidMain
backup_root=$menu/backup
lock=$menu/druid-context-policy.lock
pending=
locked=false

fail() { printf 'druid-context-policy: %s\n' "$*" >&2; exit 1; }
cleanup() {
    [ -z "$pending" ] || rm -f "$pending"
    [ "$locked" = false ] || rmdir "$lock"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Registry files on this image are unquoted strings without a trailing newline.
# Check byte count too: command substitution alone would accept extra newlines.
read_value() {
    [ -f "$1" ] && [ ! -L "$1" ] || fail "not a regular registry file: $1"
    value=$(cat "$1")
    size=$(wc -c < "$1")
    [ "$size" -eq "${#value}" ] || fail "unexpected string encoding: $1"
}
validate_target() {
    read_value "$main"
    [ "$value" = "$expected_main" ] || fail 'Context-Main mismatch; refusing another context'
    read_value "$auto"
    case "$value" in true|false) current=$value;; *) fail 'Auto-Start must be exactly true or false';; esac
}
idle_gate() {
    [ -x "$menu/hr54-ui" ] || fail 'native API readiness checker is unavailable'
    "$menu/hr54-ui" --check-api --idle >/dev/null 2>&1 ||
        fail 'API unavailable, malformed, or media/game active; no registry change'
}
usage() { printf 'Usage: %s --status | --disable | --restore BACKUP_DIRECTORY\n' "$0" >&2; exit 2; }
[ "$#" -ge 1 ] || usage
action=$1
case "$action" in
    --status|--disable) [ "$#" -eq 1 ] || usage;;
    --restore) [ "$#" -eq 2 ] || usage;;
    *) usage;;
esac
validate_target
if [ "$action" = --status ]; then
    printf 'context=%s\nconfigured_auto_start=%s\nruntime_state=not_inspected\n' "$expected_main" "$current"
    exit 0
fi
[ "$(id -u)" -eq 0 ] || fail 'registry mutation requires root'
[ -d "$menu" ] && [ ! -L "$menu" ] || fail 'native menu directory is unavailable'
mkdir "$lock" 2>/dev/null || fail "another operation or stale lock exists: $lock"
locked=true
validate_target
desired=false
restore_file=
if [ "$action" = --restore ]; then
    restore_dir=$2
    case "$restore_dir" in "$backup_root"/druid-context.*) ;; *) fail 'restore requires a policy backup directory';; esac
    # Do not accept nested paths or symlinks as policy backups.
    leaf=${restore_dir#"$backup_root"/}
    case "$leaf" in */*|*..*) fail 'invalid backup directory';; esac
    [ -d "$restore_dir" ] && [ ! -L "$restore_dir" ] || fail 'backup directory is unavailable'
    read_value "$restore_dir/target"
    [ "$value" = "$auto" ] || fail 'backup target mismatch'
    read_value "$restore_dir/Context-Main.str"
    [ "$value" = "$expected_main" ] || fail 'backup Context-Main mismatch'
    restore_file=$restore_dir/Auto-Start.str
    read_value "$restore_file"
    case "$value" in true|false) desired=$value;; *) fail 'invalid backup Auto-Start';; esac
fi
idle_gate
if [ "$current" = "$desired" ]; then
    printf 'unchanged: configured_auto_start=%s; no runtime reload performed\n' "$current"
    exit 0
fi
[ ! -L "$backup_root" ] || fail 'backup root must not be a symlink'
mkdir -p "$backup_root"
backup=$(mktemp -d "$backup_root/druid-context.XXXXXX")
cp -p "$auto" "$backup/Auto-Start.str"
cp -p "$main" "$backup/Context-Main.str"
printf '%s' "$auto" > "$backup/target"
printf '%s\n' "$action" > "$backup/action"
# Save the exact original bytes/permissions before creating the replacement.
pending=$(mktemp "$registry/.Auto-Start.native.XXXXXX")
cp -p "$auto" "$pending"
if [ -n "$restore_file" ]; then
    cat "$restore_file" > "$pending"
else
    printf '%s' "$desired" > "$pending"
fi
# Catch changed registry content and activity during backup preparation.
cmp -s "$auto" "$backup/Auto-Start.str" || fail "registry changed concurrently; backup=$backup"
cmp -s "$main" "$backup/Context-Main.str" || fail "context changed concurrently; backup=$backup"
idle_gate
mv "$pending" "$auto"
pending=
printf 'configured_auto_start=%s\nbackup=%s\n' "$desired" "$backup"
printf 'No runtime reload performed. Apply with one separately authorized normal middleware restart.\n'
