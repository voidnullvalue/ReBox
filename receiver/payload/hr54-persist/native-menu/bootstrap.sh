#!/bin/sh
# Existing late userland hook -> native API -> one guarded policy reload -> UI.
# No flash writes, Druid screen polling or input-owner impersonation.
set -eu
ui_root=/var/hr54-persist/native-menu
ui_enabled=$ui_root/ENABLED
ui_policy=$ui_root/dtv-menu-policy.car
ui_stock=/opt/dtv/dtv.car
ui_guard=/tmp/hr54-native-bootstrap.v2.attempted
ui_log=$ui_root/bootstrap.log
ui_loaded=$ui_root/loaded-boot
ui_strings=/var/hr54-persist/overlays/englishtext-755.txt
ui_string_target=/opt/ui_assets/assetspack/language/englishtext.txt
ui_suppression=$ui_root/STOCK_UI_SUPPRESSION
ui_broker=$ui_root/BROKER_ENABLED
ui_context=/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str
ui_policy_digest=2f62d0182a2c043affa69c966f240734
if [ -f "$ui_root/policy.md5" ]; then ui_policy_digest=$(cat "$ui_root/policy.md5"); fi
case "$ui_policy_digest" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#ui_policy_digest}" -eq 32 ] || exit 1
ui_wait_seconds=${HR54_BOOT_WAIT_SECONDS:-120}
case "$ui_wait_seconds" in ''|*[!0-9]*) exit 1;; esac
[ "$ui_wait_seconds" -gt 0 ] && [ "$ui_wait_seconds" -le 300 ] || exit 1
[ -f "$ui_enabled" ] && [ -f "$ui_root/ACTIVATED" ] || exit 0
now() { read ui_uptime ui_unused < /proc/uptime; printf '%s\n' "${ui_uptime%%.*}"; }
record() { printf '%s uptime=%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$(now)" "$*" >> "$ui_log"; }
digest() { md5sum "$1" | cut -d' ' -f1; }
start_frontend() {
    if [ -f "$ui_suppression" ]; then
        "$ui_root/rf-input-ready.sh" >> "$ui_log" 2>&1 || { record 'RF input service failed to initialize'; exit 1; }
    fi
    if [ -f "$ui_broker" ]; then
        [ -x "$ui_root/input-supervisor.sh" ] && [ -x "$ui_root/hr54-input-broker" ] || {
            record 'persistent input installation incomplete'; exit 1;
        }
        (trap '' HUP; exec "$ui_root/input-supervisor.sh") >> "$ui_root/input-supervisor-launch.log" 2>&1 < /dev/null &
        ui_deadline=$(($(now)+ui_wait_seconds))
        while ! "$ui_root/hr54-input-broker" --check; do
            [ "$(now)" -lt "$ui_deadline" ] || { record 'persistent input readiness deadline expired'; exit 1; }
            sleep 1
        done
        record 'persistent input owns the hardware map'
    fi
    exec "$ui_root/supervisor.sh"
}
ui_boot=$(cat /proc/sys/kernel/random/boot_id)
[ "$(digest "$ui_policy")" = "$ui_policy_digest" ] || { record 'input policy integrity failure'; exit 1; }
ui_string_digest=stock
if [ -f /var/hr54-persist/overlays/strings-boot.enabled ] && [ -f "$ui_strings" ]; then
    ui_string_digest=$(digest "$ui_strings")
fi
ui_context_policy=stock
ui_context_matches=true
if [ -f "$ui_suppression" ]; then
    ui_context_policy=native
    [ -x "$ui_root/druid-context-policy.sh" ] || { record 'stock context policy helper missing'; exit 1; }
    if [ "$(cat "$ui_context" 2>/dev/null || :)" != false ]; then ui_context_matches=false; fi
fi
ui_desired=$ui_boot:$ui_policy_digest:$ui_string_digest:$ui_context_policy
if [ "$(cat "$ui_loaded" 2>/dev/null || :)" = "$ui_desired" ] &&
   [ "$(digest "$ui_stock")" = "$ui_policy_digest" ] && [ "$ui_context_matches" = true ]; then
    record 'policy already loaded this boot; starting/rejoining UI supervisor'
    start_frontend
fi
mkdir "$ui_guard" 2>/dev/null || { record 'bootstrap already attempted this boot; no repeated middleware reload'; exit 1; }
ui_deadline=$(($(now)+ui_wait_seconds))
while [ -f "$ui_enabled" ]; do
    if pidof dtvwm >/dev/null && pidof keydispatcher >/dev/null && pidof siege >/dev/null &&
       "$ui_root/hr54-ui" --check-api --idle; then break; fi
    [ "$(now)" -lt "$ui_deadline" ] || { record 'services unavailable or media busy; readiness deadline expired'; exit 1; }
    sleep 2
done
[ -f "$ui_enabled" ] || exit 0
if pidof hr54-ui >/dev/null; then
    record 'running native shell must be stopped cleanly before a policy reload'
    exit 1
fi
case "$(digest "$ui_stock")" in
    882124071cfe3ac1740af18161995dcf|2f62d0182a2c043affa69c966f240734|"$ui_policy_digest") :;;
    *) record 'unsupported stock build; retaining stock'; exit 1;;
esac
if [ "$ui_context_policy" = native ]; then
    "$ui_root/druid-context-policy.sh" --disable >> "$ui_log" 2>&1 || {
        record 'stock context policy refused; no middleware reload'; exit 1;
    }
    record 'stock interactive context disabled for next middleware start'
fi
record 'loading input/context policy and saved strings through one middleware reload'
dt stop >> "$ui_log" 2>&1
if [ "$(digest "$ui_stock")" != "$ui_policy_digest" ]; then
    # An older per-file bind can be busy while middleware is live.  Stop the
    # coordinated stack once before replacing that mount; never add a second
    # reload merely to change policy revisions.
    if [ "$(digest "$ui_stock")" != 882124071cfe3ac1740af18161995dcf ]; then umount "$ui_stock"; fi
    mount --bind "$ui_policy" "$ui_stock"
fi
if [ "$ui_string_digest" != stock ] && [ "$(digest "$ui_string_target")" != "$ui_string_digest" ]; then
    mount --bind "$ui_strings" "$ui_string_target"
fi
(trap '' HUP; exec dt run) >> "$ui_root/middleware.log" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$ui_root/middleware.pid"
ui_deadline=$(($(now)+ui_wait_seconds))
while [ -f "$ui_enabled" ]; do
    if pidof dtvwm >/dev/null && pidof keydispatcher >/dev/null &&
       { [ -f "$ui_broker" ] || "$ui_root/hr54-ui" --probe-input > "$ui_root/input-probe.log" 2>&1; }; then
        printf '%s\n' "$ui_desired" > "$ui_loaded.next"
        chmod 600 "$ui_loaded.next"
        mv "$ui_loaded.next" "$ui_loaded"
        record 'middleware ready after reload; starting native input and UI'
        start_frontend
    fi
    [ "$(now)" -lt "$ui_deadline" ] || break
    sleep 2
done
# The legacy path retains its historical stock recovery. Native-exclusive
# installations keep the context disabled for explicit, backed-up recovery.
if [ "$ui_context_policy" = native ] || [ -f "$ui_broker" ]; then
    record 'native input readiness failed; retaining policy for explicit recovery'
    exit 1
fi
record 'native routing failed; restoring stock policy with one recovery reload'
umount "$ui_stock" 2>/dev/null || :
dt stop >> "$ui_log" 2>&1
(trap '' HUP; exec dt run) >> "$ui_root/middleware-recovery.log" 2>&1 < /dev/null &
exit 1
