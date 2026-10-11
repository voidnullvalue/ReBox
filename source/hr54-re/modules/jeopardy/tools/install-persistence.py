#!/usr/bin/env python3
"""Guarded, one-shot install of the Jeopardy reboot-persistence additions.

This tool deploys only artifacts that live *outside* the module package and
cannot be installed by the module runtime:

* the module-owned firewall boot hook (`boot.d/jeopardy-network.sh`, 0700);
* its private, root-owned scope config (`boot.d/jeopardy-network.conf`, 0600);
* the reviewed additive launcher (`/var/hr54-persist/rebox-start.sh`) that
  invokes generic boot.d hooks.

It never touches the core binary, the module package/code, other modules,
firmware, vendor content, stock bootstrap or pairing state, and it never
restarts, stops, resets or reboots anything or runs the whole launcher live.
Only the isolated boot hook is executed, and only idempotently.

Guard rails:

* the local artifacts are pinned by SHA-256 and checked, with expected markers,
  before anything touches the receiver;
* the remote launcher hash is compared against the known-good baseline and the
  reviewed replacement. An unknown (manually edited) launcher causes a STOP with
  no overwrite;
* the existing launcher is backed up (timestamp + module version) before any
  atomic, fsync'd replacement, and a generated rollback routine is written;
* hook/config are written atomically, 0700/0600, idempotently, with backups;
* before/after probes prove the core pid, native pid, current players and LAN
  health are unchanged and that exactly one scoped firewall rule exists.

Usage:
    python3 source/hr54-re/modules/jeopardy/tools/install-persistence.py \
        --host 192.168.88.103 --interface eth0 --lan 192.168.88.0/24
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
LAUNCHER = HERE.parents[2] / "reboxd" / "rebox-start.sh"
HOOK = HERE / "jeopardy-network.sh"
REPO_ROOT = HERE.parents[4]

DEFAULT_SHELL = REPO_ROOT / "tools" / "recv" / "hr54-shell.sh"
DEFAULT_PUSH = REPO_ROOT / "tools" / "recv" / "hr54-push.sh"
STAGE_REMOTE = "/tmp/jpq-persist-stage"
AGENT_REMOTE = "/tmp/jpq-persist-agent.py"
RECEIVER_PYTHON_HOME = "/var/hr54-persist/rebox/module-data/youtube/youtube/python"
RECEIVER_PYTHON = "/var/hr54-persist/rebox/module-data/youtube/youtube/bin/python"

# Reviewed content pinned by digest (see tools/jeopardy-network.sh and
# source/hr54-re/reboxd/rebox-start.sh).
EXPECTED_LAUNCHER_SHA256 = "76e282f95c5fa943146f66bd01a8d9b948ae37f7a67fde6797f6b3bb27cf2442"
EXPECTED_HOOK_SHA256 = "524b597df14818ee99bd1badaeef890faf7a5f56b22079e0f304c764067aa801"
KNOWN_LAUNCHER_SHA256 = {
    "e3de6ec6f6ccde6b5487ae76912c3ceff1079c889f351b6572a67db31e0b8506": "Oct 7 baseline",
    EXPECTED_LAUNCHER_SHA256: "boot.d hook launcher",
}
LAUNCHER_MARKERS = ["bootd=$runtime/boot.d", "$runtime/log/boot-hook-"]
HOOK_MARKERS = ["JEOPARDY-LAN-BOOT-HOOK"]

AGENT = r'''#!/usr/bin/env python3
import hashlib, json, os, stat, subprocess, sys, tempfile, urllib.request

BASE = "/var/hr54-persist"
RUNTIME = os.path.join(BASE, "rebox")
STATE = os.path.join(RUNTIME, "module-state/jeopardy.json")
MANIFEST = os.path.join(RUNTIME, "modules/jeopardy/module.json")
BOOTD = os.path.join(RUNTIME, "boot.d")
HOOK = os.path.join(BOOTD, "jeopardy-network.sh")
CONF = os.path.join(BOOTD, "jeopardy-network.conf")
LAUNCHER = os.path.join(BASE, "rebox-start.sh")
BACKUPROOT = os.path.join(BASE, "backup")
SESSIONS = os.path.join(RUNTIME, "module-data/jeopardy/jeopardy-sessions.json")
NATIVE_PID = os.path.join(RUNTIME, "module-data/jeopardy/native/jeopardy.pid")


def emit(status, payload):
    print("REBOX_RESULT %s %s" % (status, json.dumps(payload, sort_keys=True)))


def sha256(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 16), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def mode(path):
    try:
        return "%03o" % stat.S_IMODE(os.stat(path).st_mode)
    except OSError:
        return ""


def pid_alive(path):
    try:
        pid = int(open(path).read().strip())
    except (OSError, ValueError):
        return 0
    return pid if pid > 1 and os.path.exists("/proc/%d" % pid) else 0


def players():
    # Only non-secret fields: the ids and the next id. Tokens are never read out.
    try:
        with open(SESSIONS) as f:
            d = json.load(f)
        ids = sorted(int(p.get("id", -1)) for p in d.get("players", []))
        return {"nextPlayerId": d.get("nextPlayerId"), "players": ids}
    except (OSError, ValueError):
        return {"nextPlayerId": None, "players": []}


def rule_count(iface, lan, port):
    want = "-A INPUT -s %s -i %s -p tcp -m tcp --dport %s -j ACCEPT" % (lan, iface, port)
    try:
        out = subprocess.run(["iptables", "-S", "INPUT"], capture_output=True,
                             text=True, timeout=10).stdout
    except OSError:
        return -1
    return sum(1 for line in out.splitlines() if line.rstrip() == want)


def health():
    try:
        with urllib.request.urlopen("http://127.0.0.1:8135/health", timeout=5) as r:
            return r.status, len(r.read())
    except Exception:
        return 0, 0


def probe(iface, lan, port):
    code, length = health()
    return {
        "launcher_sha": sha256(LAUNCHER), "launcher_mode": mode(LAUNCHER),
        "hook_sha": sha256(HOOK), "hook_mode": mode(HOOK),
        "conf_sha": sha256(CONF), "conf_mode": mode(CONF),
        "core_pid": pid_alive(os.path.join(RUNTIME, "reboxd.pid")),
        "native_pid": pid_alive(NATIVE_PID),
        "public_sha": sha256(os.path.join(RUNTIME, "module-data/jeopardy/native/public-display.json")),
        "players": players(),
        "rule_count": rule_count(iface, lan, port),
        "health_code": code, "health_len": length,
        "state": open(STATE).read().strip() if os.path.exists(STATE) else "",
        "manifest_sha": sha256(MANIFEST),
    }


def atomic_write(path, data, mode_bits):
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d, 0o700)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".jpq-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode_bits)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    dfd = os.open(d, os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


def backup_file(src, dest):
    if not os.path.exists(src):
        return False
    with open(src, "rb") as f:
        data = f.read()
    st = os.stat(src)
    d = os.path.dirname(dest)
    if not os.path.isdir(d):
        os.makedirs(d, 0o700)
    with open(dest, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(dest, stat.S_IMODE(st.st_mode))
    try:
        os.utime(dest, (st.st_atime, st.st_mtime))
    except OSError:
        pass
    return True


def write_rollback(backup_dir, iface, lan, port, ts):
    lines = [
        "#!/bin/sh",
        "# Generated rollback for the Jeopardy persistence install %s." % ts,
        "set -eu",
        "base=/var/hr54-persist",
        "backup=%s" % backup_dir,
        '[ -d "$backup" ] || exit 1',
        'if [ -f "$backup/rebox-start.sh" ]; then',
        '    cp "$backup/rebox-start.sh" "$base/rebox-start.sh.jpq-restore"',
        '    chmod 700 "$base/rebox-start.sh.jpq-restore"',
        '    mv "$base/rebox-start.sh.jpq-restore" "$base/rebox-start.sh"',
        "fi",
        'if [ -f "$backup/jeopardy-network.sh" ]; then',
        '    cp "$backup/jeopardy-network.sh" "$base/rebox/boot.d/jeopardy-network.sh"',
        '    chmod 700 "$base/rebox/boot.d/jeopardy-network.sh"',
        "else",
        '    rm -f "$base/rebox/boot.d/jeopardy-network.sh"',
        "fi",
        'if [ -f "$backup/jeopardy-network.conf" ]; then',
        '    cp "$backup/jeopardy-network.conf" "$base/rebox/boot.d/jeopardy-network.conf"',
        '    chmod 600 "$base/rebox/boot.d/jeopardy-network.conf"',
        "else",
        '    rm -f "$base/rebox/boot.d/jeopardy-network.conf"',
        "fi",
        "iptables -D INPUT -i %s -s %s -p tcp --dport %s -j ACCEPT 2>/dev/null || true" % (iface, lan, port),
        'echo "Jeopardy persistence rollback complete"',
    ]
    atomic_write(os.path.join(backup_dir, "rollback.sh"),
                 ("\n".join(lines) + "\n").encode(), 0o700)


def apply(stage, launcher_sha, hook_sha, known_old, ts, iface, lan, port):
    new_launcher = open(os.path.join(stage, "rebox-start.sh"), "rb").read()
    new_hook = open(os.path.join(stage, "jeopardy-network.sh"), "rb").read()
    new_conf = open(os.path.join(stage, "jeopardy-network.conf"), "rb").read()
    for name, want in (("rebox-start.sh", launcher_sha), ("jeopardy-network.sh", hook_sha)):
        got = sha256(os.path.join(stage, name))
        if got != want:
            emit(400, {"error": "staged %s mismatch" % name, "want": want, "got": got})
            return
    remote_sha = sha256(LAUNCHER)
    if remote_sha != launcher_sha and remote_sha not in known_old:
        emit(409, {"error": "unknown remote launcher hash; refusing to overwrite",
                   "remote_sha": remote_sha,
                   "known": sorted(set(known_old) | {launcher_sha})})
        return

    changed_launcher = remote_sha != launcher_sha
    hook_changes = sha256(HOOK) != hook_sha
    conf_changes = sha256(CONF) != sha256_bytes(new_conf)
    any_change = changed_launcher or hook_changes or conf_changes
    backup_dir = os.path.join(BACKUPROOT, "jeopardy-persistence-%s" % ts)
    actions = []
    version = ""
    try:
        version = json.load(open(MANIFEST)).get("version", "")
    except Exception:
        pass

    if any_change and not os.path.isdir(backup_dir):
        os.makedirs(backup_dir, 0o700)

    if changed_launcher:
        backup_file(LAUNCHER, os.path.join(backup_dir, "rebox-start.sh"))
        atomic_write(LAUNCHER, new_launcher, 0o700)
        actions.append("launcher deployed (backup %s)" % backup_dir)
    else:
        actions.append("launcher already current")

    if hook_changes:
        backup_file(HOOK, os.path.join(backup_dir, "jeopardy-network.sh"))
        atomic_write(HOOK, new_hook, 0o700)
        actions.append("boot hook installed")
    else:
        os.chmod(HOOK, 0o700)
        actions.append("boot hook already current")

    if conf_changes:
        backup_file(CONF, os.path.join(backup_dir, "jeopardy-network.conf"))
        atomic_write(CONF, new_conf, 0o600)
        actions.append("boot config installed")
    else:
        os.chmod(CONF, 0o600)
        actions.append("boot config already current")

    if any_change:
        write_rollback(backup_dir, iface, lan, port, ts)
        try:
            with open(os.path.join(backup_dir, "manifest.json"), "w") as f:
                json.dump({"timestamp": ts, "module": "jeopardy", "version": version,
                           "old_launcher_sha256": remote_sha,
                           "new_launcher_sha256": launcher_sha,
                           "hook_sha256": hook_sha, "interface": iface,
                           "lan": lan, "port": port}, f, indent=2, sort_keys=True)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(os.path.join(backup_dir, "manifest.json"), 0o600)
        except OSError:
            pass
        actions.append("rollback routine written")

    env = dict(os.environ, PATH="/bin:/sbin:/usr/bin:/usr/sbin")
    rc = subprocess.run(["/bin/sh", HOOK], capture_output=True, text=True, env=env, timeout=30)
    actions.append("hook run rc=%d" % rc.returncode)
    emit(200, {"ok": True, "actions": actions, "backup": backup_dir if any_change else "",
               "launcher_changed": changed_launcher, "hook_output": rc.stdout.strip(),
               "rule_count": rule_count(iface, lan, port),
               "launcher_sha": sha256(LAUNCHER), "hook_sha": sha256(HOOK),
               "hook_mode": mode(HOOK), "conf_mode": mode(CONF)})


def main():
    if len(sys.argv) < 2:
        print("usage: agent probe|apply ...", file=sys.stderr)
        return 2
    mode = sys.argv[1]
    if mode == "probe":
        emit(200, probe(sys.argv[2], sys.argv[3], sys.argv[4]))
    elif mode == "apply":
        stage, lsha, hsha, old, ts, iface, lan, port = sys.argv[2:10]
        apply(stage, lsha, hsha, [x for x in old.split(",") if x], ts, iface, lan, port)
    else:
        emit(400, {"error": "unknown mode %s" % mode})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def shell_run(shell: pathlib.Path, host: str, command: str, timeout: int = 180,
              drain: str = "3") -> str:
    env = dict(os.environ, HR54_HOST=host, HR54_TIMEOUT=str(timeout), HR54_DRAIN=drain)
    p = subprocess.run([str(shell), command], capture_output=True, text=True,
                       timeout=timeout + 60, env=env)
    return p.stdout + p.stderr


def push_file(push: pathlib.Path, local: pathlib.Path, remote: str) -> None:
    p = subprocess.run([str(push), str(local), remote], capture_output=True, text=True,
                       timeout=180)
    out = p.stdout + p.stderr
    if p.returncode != 0 or remote not in out or "No such file" in out:
        raise RuntimeError("push failed for %s:\n%s" % (remote, out.strip()))


def agent(shell: pathlib.Path, host: str, mode: str, *extra: str) -> tuple[int, dict, str]:
    args = " ".join(shlex.quote(a) for a in (mode,) + extra)
    cmd = "PYTHONHOME=%s %s %s %s" % (RECEIVER_PYTHON_HOME, RECEIVER_PYTHON, AGENT_REMOTE, args)
    out = shell_run(shell, host, cmd)
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("REBOX_RESULT "):
            _, code, payload = line.split(" ", 2)
            return int(code), json.loads(payload), out
    raise RuntimeError("no REBOX_RESULT from receiver agent:\n%s" % out.strip())


def lan_health(host: str, port: int) -> tuple[int, str]:
    try:
        with urllib.request.urlopen("http://%s:%d/health" % (host, port), timeout=6) as r:
            return r.status, r.read().decode(errors="replace")
    except Exception as exc:  # noqa: BLE001 - reported, never fatal here
        return 0, str(exc)


def build_config(iface: str, lan: str, port: int) -> bytes:
    return (
        "# Jeopardy module LAN firewall boot hook config (root-owned, 0600).\n"
        "# Deployed outside the module package by tools/install-persistence.py;\n"
        "# never installed by the module runtime.\n"
        "INTERFACE=%s\nLAN=%s\nPORT=%d\n" % (iface, lan, port)
    ).encode()


def viable(iface: str, lan: str, port: int) -> str | None:
    if not iface or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-" for c in iface):
        return "invalid --interface"
    if "/" not in lan or any(c not in "0123456789./" for c in lan):
        return "invalid --lan"
    prefix = lan.split("/", 1)[1]
    if not prefix.isdigit() or not (1 <= int(prefix) <= 32):
        return "invalid --lan prefix"
    if not (1 <= port <= 65535):
        return "invalid --port"
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.88.103", help="receiver IPv4")
    ap.add_argument("--interface", default="eth0", help="wired LAN interface")
    ap.add_argument("--lan", default="192.168.88.0/24", help="private LAN CIDR")
    ap.add_argument("--port", type=int, default=8135, help="module LAN TCP port")
    ap.add_argument("--shell", default=str(DEFAULT_SHELL), help="hr54-shell.sh transport")
    ap.add_argument("--push", default=str(DEFAULT_PUSH), help="hr54-push.sh file transport")
    ap.add_argument("--report", default=None, help="optional path to write a sanitized report")
    ap.add_argument("--dry-run", action="store_true", help="verify and probe only; deploy nothing")
    args = ap.parse_args()

    bad = viable(args.interface, args.lan, args.port)
    if bad:
        print("FAIL: %s" % bad, file=sys.stderr)
        return 2

    # 1. Verify the reviewed local artifacts and their expected markers.
    if not LAUNCHER.is_file() or not HOOK.is_file():
        print("FAIL: local artifacts missing", file=sys.stderr)
        return 2
    launcher_sha = sha256_file(LAUNCHER)
    hook_sha = sha256_file(HOOK)
    if launcher_sha != EXPECTED_LAUNCHER_SHA256:
        print("FAIL: local launcher sha %s != pinned %s" % (launcher_sha, EXPECTED_LAUNCHER_SHA256),
              file=sys.stderr)
        return 2
    if hook_sha != EXPECTED_HOOK_SHA256:
        print("FAIL: local hook sha %s != pinned %s" % (hook_sha, EXPECTED_HOOK_SHA256),
              file=sys.stderr)
        return 2
    launcher_src = LAUNCHER.read_text()
    hook_src = HOOK.read_text()
    for marker in LAUNCHER_MARKERS:
        if marker not in launcher_src:
            print("FAIL: launcher marker missing: %s" % marker, file=sys.stderr)
            return 2
    for marker in HOOK_MARKERS:
        if marker not in hook_src:
            print("FAIL: hook marker missing: %s" % marker, file=sys.stderr)
            return 2

    shell = pathlib.Path(args.shell).resolve()
    push = pathlib.Path(args.push).resolve()
    for tool in (shell, push):
        if not tool.is_file():
            print("FAIL: transport tool missing: %s" % tool, file=sys.stderr)
            return 2

    report: list[str] = []
    report.append("# Jeopardy reboot persistence - guarded install to HR54 %s" % args.host)
    report.append("")
    report.append("- launcher: `%s` (sha256 `%s`)" % (LAUNCHER.name, launcher_sha))
    report.append("- hook: `%s` (sha256 `%s`)" % (HOOK.name, hook_sha))
    report.append("- scope: interface `%s`, LAN `%s`, TCP `%d`" % (args.interface, args.lan, args.port))
    report.append("")

    # 2. Upload the guarded agent, then take a read-only pre-probe.
    with tempfile.NamedTemporaryFile("w", suffix="-agent.py", delete=False) as tmp:
        tmp.write(AGENT)
        agent_local = pathlib.Path(tmp.name)
    try:
        push_file(push, agent_local, AGENT_REMOTE)
    finally:
        agent_local.unlink(missing_ok=True)

    try:
        code, pre, raw = agent(shell, args.host, "probe", args.interface, args.lan, str(args.port))
        if code != 200:
            print("FAIL: pre-probe HTTP %d: %s" % (code, pre), file=sys.stderr)
            return 1
        print("pre-probe: launcher_sha=%s hook_present=%s rule_count=%s core_pid=%s native_pid=%s players=%s"
              % (pre["launcher_sha"][:12], bool(pre["hook_sha"]), pre["rule_count"],
                 pre["core_pid"], pre["native_pid"], pre["players"]))
        report.append("- pre: launcher `%s`, rule_count `%s`, core_pid `%s`, native_pid `%s`, players `%s`"
                      % (pre["launcher_sha"], pre["rule_count"], pre["core_pid"],
                         pre["native_pid"], pre["players"]))

        if pre["launcher_sha"] not in KNOWN_LAUNCHER_SHA256:
            print("ABORT: remote launcher hash is unknown (manual changes present); not overwriting.\n"
                  "  remote: %s" % pre["launcher_sha"], file=sys.stderr)
            report.append("- ABORTED: unknown remote launcher hash `%s`" % pre["launcher_sha"])
            return 3

        if args.dry_run:
            plan = ("would deploy" if pre["launcher_sha"] != launcher_sha else "launcher already current")
            print("dry-run: %s; hook sha %s; rule_count %s" % (plan, hook_sha[:12], pre["rule_count"]))
            return 0

        # 3. Stage the reviewed artifacts on the receiver and run the guarded agent.
        ts = time.strftime("%Y%m%d-%H%M%S")
        shell_run(shell, args.host, "rm -rf %s && mkdir -p %s && chmod 700 %s && echo STAGE_OK"
                  % (STAGE_REMOTE, STAGE_REMOTE, STAGE_REMOTE))
        push_file(push, LAUNCHER, STAGE_REMOTE + "/rebox-start.sh")
        push_file(push, HOOK, STAGE_REMOTE + "/jeopardy-network.sh")
        config = build_config(args.interface, args.lan, args.port)
        with tempfile.NamedTemporaryFile("wb", suffix="-jeopardy-network.conf", delete=False) as tmp:
            tmp.write(config)
            cfg_local = pathlib.Path(tmp.name)
        try:
            push_file(push, cfg_local, STAGE_REMOTE + "/jeopardy-network.conf")
        finally:
            cfg_local.unlink(missing_ok=True)
        known = ",".join(sorted(KNOWN_LAUNCHER_SHA256))
        code, result, raw = agent(shell, args.host, "apply", STAGE_REMOTE, launcher_sha,
                                  hook_sha, known, ts, args.interface, args.lan, str(args.port))
        print("apply HTTP %d: %s" % (code, result))
        report.append("- apply HTTP %d: `%s`" % (code, result))
        if code != 200:
            print("FAIL: apply rejected", file=sys.stderr)
            return 1

        # 4. Read-only post-probe and invariants.
        code, post, raw = agent(shell, args.host, "probe", args.interface, args.lan, str(args.port))
        if code != 200:
            print("FAIL: post-probe HTTP %d: %s" % (code, post), file=sys.stderr)
            return 1
        health_code, health_body = lan_health(args.host, args.port)
        report.append("- post: launcher `%s`, hook_mode `%s`, conf_mode `%s`, rule_count `%s`"
                      % (post["launcher_sha"], post["hook_mode"], post["conf_mode"], post["rule_count"]))
        report.append("- post: core_pid `%s`, native_pid `%s`, players `%s`, LAN health HTTP `%s`"
                      % (post["core_pid"], post["native_pid"], post["players"], health_code))

        ok = True

        def inv(name: str, condition: bool, detail: str = "") -> None:
            nonlocal ok
            print("%-4s %s%s" % ("PASS" if condition else "FAIL", name,
                                 (" - " + detail) if detail and not condition else ""))
            if not condition:
                ok = False

        inv("persistent launcher hash is the reviewed content", post["launcher_sha"] == launcher_sha,
            post["launcher_sha"])
        inv("persistent launcher mode 700", post["launcher_mode"] == "700", post["launcher_mode"])
        inv("boot hook hash is the reviewed content", post["hook_sha"] == hook_sha, post["hook_sha"])
        inv("boot hook mode 700", post["hook_mode"] == "700", post["hook_mode"])
        inv("boot config mode 600", post["conf_mode"] == "600", post["conf_mode"])
        inv("exactly one scoped firewall rule", post["rule_count"] == 1, str(post["rule_count"]))
        inv("core pid unchanged", post["core_pid"] == pre["core_pid"] and post["core_pid"] != 0,
            "%s -> %s" % (pre["core_pid"], post["core_pid"]))
        inv("native pid unchanged", post["native_pid"] == pre["native_pid"] and post["native_pid"] != 0,
            "%s -> %s" % (pre["native_pid"], post["native_pid"]))
        inv("current players unchanged", post["players"] == pre["players"],
            "%s -> %s" % (pre["players"], post["players"]))
        inv("module state unchanged (schema 1 enabled installed)",
            post["state"] == pre["state"] and '"schema":1' in post["state"], post["state"])
        inv("LAN health reachable", health_code == 200, "HTTP %s %s" % (health_code, health_body[:80]))
        report.append("- checks: %s" % ("all passed" if ok else "FAILED"))

        print("backup: %s" % (result.get("backup") or "(none; launcher already current)"))
        if result.get("backup"):
            print("rollback: %s" % os.path.join(result["backup"], "rollback.sh"))
        if not ok:
            print("FAIL: post-install invariants not met", file=sys.stderr)
            return 1
        print("PERSISTENCE INSTALL: OK")
        return 0
    finally:
        _write_report(args.report, report)
        shell_run(shell, args.host, "rm -f %s; rm -rf %s" % (AGENT_REMOTE, STAGE_REMOTE), timeout=30)


def _write_report(path: str | None, lines: list[str]) -> None:
    if not path:
        return
    p = pathlib.Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
