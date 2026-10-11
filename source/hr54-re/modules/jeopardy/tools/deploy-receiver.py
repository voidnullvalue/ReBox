#!/usr/bin/env python3
"""Guarded module-only install of the reviewed Jeopardy .rbox to a live HR54.

This tool installs exactly one already-built module package through the core
management transaction (`POST /api/modules/install`). It never edits core
source, the builtin catalog, firmware or any other module, and it never pairs,
revokes or changes Android management authorization.

Guard rails:

* the local package SHA-256 is verified before anything touches the receiver;
* the receiver must be idle (no active media playback, no native surface owned
  by any module, no native-app child running) before the install starts; a busy
  receiver is reported and left untouched - an in-progress session is never
  killed;
* the package is served from the development host over a short-lived LAN HTTP
  server that is shut down even on failure;
* the management bearer is read from the receiver's own
  `module-state/management.secret` by a script executed on the receiver via the
  existing `hr54-shell.sh` root transport. The bearer stays on the receiver and
  is never printed, logged or passed on a command line.

The receiver has no curl CLI; the on-box HTTP client is the module runtime's
CPython (`urllib`), invoked over the same root shell. That keeps the secret
local to the receiver.

Usage:
    python3 tools/deploy-receiver.py --host 192.168.88.103 \
        --package build/jeopardy.rbox \
        --sha256 cbfe4c3f206f8444aa034a44197da30f63eff1f1e68c0fa42919a56156e4ed29
"""
from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading

HERE = pathlib.Path(__file__).resolve().parent
MODULE_ROOT = HERE.parent
REPO_ROOT = HERE.parents[4]  # .../jeopardy/tools -> repo root

DEFAULT_SHELL = REPO_ROOT / "tools" / "recv" / "hr54-shell.sh"
DEFAULT_PUSH = REPO_ROOT / "tools" / "recv" / "hr54-push.sh"
AGENT_REMOTE = "/tmp/rebox-jeopardy-deploy-agent.py"
SECRET_PATH = "/var/hr54-persist/rebox/module-state/management.secret"
RECEIVER_PYTHON_HOME = "/var/hr54-persist/rebox/module-data/youtube/youtube/python"
RECEIVER_PYTHON = "/var/hr54-persist/rebox/module-data/youtube/youtube/bin/python"

# Executed *on the receiver*. Reads the management secret locally; never prints
# it. Argv carries only the non-secret LAN url and the pinned digest.
AGENT = r'''#!/usr/bin/env python3
import json, sys, urllib.request, urllib.error

PORT = int(sys.argv[1])
SECRET_PATH = sys.argv[2]
MODE = sys.argv[3]


def api(path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=20) as r:
        return r.status, json.load(r)


def emit(status, payload):
    print("REBOX_RESULT %s %s" % (status, json.dumps(payload, sort_keys=True)))


def idle():
    _, state = api("/api/state")
    _, sysst = api("/api/system/status")
    _, reg = api("/api/modules")
    native_running = []
    for m in reg.get("modules", []):
        if not m.get("capabilities", {}).get("nativeApp"):
            continue
        try:
            _, st = api("/api/modules/%s/native/status" % m["id"])
            if st.get("running"):
                native_running.append(m["id"])
        except Exception:
            pass
    media_busy = bool(state.get("playing") or state.get("preparing")) or bool(sysst.get("mediaBusy"))
    owner = sysst.get("nativeModule", "")
    emit(200, {
        "idle": bool(not media_busy and not owner and not native_running),
        "mediaBusy": media_busy,
        "nativeOwner": owner,
        "nativeRunning": native_running,
        "state": state,
    })


def install():
    url, sha = sys.argv[4], sys.argv[5]
    with open(SECRET_PATH) as f:
        secret = f.read().strip()
    body = json.dumps({"url": url, "sha256": sha}).encode()
    req = urllib.request.Request(
        "http://127.0.0.1:%d/api/modules/install" % PORT,
        data=body,
        headers={"Authorization": "Bearer " + secret, "Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            emit(r.status, json.loads(r.read().decode() or "{}"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        try:
            payload = json.loads(raw or "{}")
        except ValueError:
            payload = {"error": raw}
        emit(e.code, payload)


def modules():
    st, reg = api("/api/modules")
    emit(st, reg)


def system():
    st, stt = api("/api/system/status")
    emit(st, stt)


MODES = {"idle": idle, "install": install, "modules": modules, "system": system}
MODES[MODE]()
'''


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def detect_lan_ip(host: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def run(cmd: list[str], timeout: int = 240) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def receiver_agent(shell: pathlib.Path, host: str, core_port: int, mode: str,
                   *extra: str) -> dict:
    """Invoke the on-receiver agent over the root shell transport."""
    args = " ".join("'%s'" % a for a in (str(core_port), SECRET_PATH, mode) + extra)
    remote = ("PYTHONHOME=%s %s %s %s" %
              (RECEIVER_PYTHON_HOME, RECEIVER_PYTHON, AGENT_REMOTE, args))
    env = dict(os.environ, HR54_HOST=host, HR54_TIMEOUT="220", HR54_DRAIN="2")
    proc = subprocess.run([str(shell), remote], capture_output=True, text=True,
                          timeout=300, env=env)
    out = proc.stdout + proc.stderr
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("REBOX_RESULT "):
            _, code, payload = line.split(" ", 2)
            return {"code": int(code), "payload": json.loads(payload), "raw": line}
    raise RuntimeError("no REBOX_RESULT from receiver agent;\n" + out.strip())


class PackageServer:
    """Serve one file to the receiver over a short-lived LAN HTTP server."""

    def __init__(self, bind: str, package: pathlib.Path, path: str):
        self.path = path
        self.hits = 0
        package_bytes = package.read_bytes()

        hits = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path != path:
                    self.send_error(404)
                    return
                hits.hits += 1
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(package_bytes)))
                self.end_headers()
                self.wfile.write(package_bytes)

        self.server = http.server.ThreadingHTTPServer((bind, 0), Handler)

    def __enter__(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.88.103", help="receiver IPv4")
    ap.add_argument("--package", required=True, help="local .rbox to install")
    ap.add_argument("--sha256", required=True, help="pinned lowercase SHA-256 of the package")
    ap.add_argument("--core-port", type=int, default=8130, help="core HTTP port on the receiver")
    ap.add_argument("--lan-host", default=None,
                    help="development host LAN IPv4 to serve from (default: auto-detect)")
    ap.add_argument("--http-port", type=int, default=0, help="serving port (0 = ephemeral)")
    ap.add_argument("--shell", default=str(DEFAULT_SHELL), help="hr54-shell.sh transport")
    ap.add_argument("--push", default=str(DEFAULT_PUSH), help="hr54-push.sh file transport")
    ap.add_argument("--report", default=None, help="optional path to write a sanitized markdown report")
    ap.add_argument("--skip-idle-check", action="store_true",
                    help="do not abort on a busy receiver (still reported)")
    ap.add_argument("--dry-run", action="store_true", help="verify and print the plan, install nothing")
    args = ap.parse_args()

    package = pathlib.Path(args.package).resolve()
    if not package.is_file():
        print("FAIL: package not found: %s" % package, file=sys.stderr)
        return 2
    digest = sha256_file(package)
    if digest != args.sha256:
        print("FAIL: package SHA-256 mismatch\n  expected %s\n  actual   %s"
              % (args.sha256, digest), file=sys.stderr)
        return 2
    shell = pathlib.Path(args.shell).resolve()
    push = pathlib.Path(args.push).resolve()
    for tool in (shell, push):
        if not tool.is_file():
            print("FAIL: transport tool missing: %s" % tool, file=sys.stderr)
            return 2

    lan_host = args.lan_host or detect_lan_ip(args.host)
    report: list[str] = []
    report.append("# Jeopardy module - guarded install to HR54 %s" % args.host)
    report.append("")
    report.append("- package: `%s`" % package.name)
    report.append("- sha256: `%s`" % digest)
    report.append("- core port: %d, serving host: %s" % (args.core_port, lan_host))
    report.append("")

    # Upload the on-receiver agent (contains no secret, no third-party data).
    with tempfile.NamedTemporaryFile("w", suffix="-agent.py", delete=False) as tmp:
        tmp.write(AGENT)
        agent_local = pathlib.Path(tmp.name)

    try:
        up = run([str(push), str(agent_local), AGENT_REMOTE], timeout=120)
        up_out = up.stdout + up.stderr
        if AGENT_REMOTE not in up_out or "No such file" in up_out:
            print("FAIL: agent upload failed:\n%s" % up_out, file=sys.stderr)
            return 1

        # 1. Receiver must be idle before we touch it.
        idle = receiver_agent(shell, args.host, args.core_port, "idle")
        summary = idle["payload"]
        print("receiver idle: %s (mediaBusy=%s nativeOwner=%r nativeRunning=%s)"
              % (summary["idle"], summary["mediaBusy"], summary["nativeOwner"],
                 summary["nativeRunning"]))
        report.append("- receiver idle before install: `%s` (mediaBusy=%s, nativeOwner=%r, nativeRunning=%s)"
                      % (summary["idle"], summary["mediaBusy"], summary["nativeOwner"],
                         summary["nativeRunning"]))
        if not summary["idle"] and not args.skip_idle_check:
            print("ABORT: receiver is busy; refusing to interrupt an active session.",
                  file=sys.stderr)
            report.append("- ABORTED: receiver busy")
            _write_report(args.report, report)
            return 3

        if args.dry_run:
            print("dry-run: verified %s (%s); no install performed." % (package, digest))
            _write_report(args.report, report)
            return 0

        # 2. Serve the package from the development host LAN address.
        url_path = "/jeopardy-%s.rbox" % digest[:12]
        with PackageServer(lan_host, package, url_path) as server:
            url = "http://%s:%d%s" % (lan_host, server.server.server_port, url_path)
            print("serving %s at %s" % (package.name, url))
            report.append("- package URL: `%s`" % url)
            # 3. Install through the guarded core transaction using the
            #    receiver-local management secret (never printed).
            result = receiver_agent(shell, args.host, args.core_port, "install", url, digest)
            print("install HTTP %d: %s" % (result["code"], result["payload"]))
            report.append("- install response: HTTP %d `%s`" % (result["code"], result["payload"]))
            fetched = server.hits
        report.append("- receiver fetches of the package (dev-host): %d" % fetched)
        if result["code"] != 200:
            print("FAIL: install rejected", file=sys.stderr)
            _write_report(args.report, report)
            return 1

        # 4. Confirm the registry sees a healthy install.
        mods = receiver_agent(shell, args.host, args.core_port, "modules")["payload"]
        entry = next((m for m in mods.get("modules", []) if m.get("id") == "jeopardy"), None)
        if not entry:
            print("FAIL: jeopardy absent from /api/modules", file=sys.stderr)
            _write_report(args.report, report)
            return 1
        print("installed: %s version=%s installed=%s enabled=%s healthy=%s core=%s"
              % (entry.get("id"), entry.get("version"), entry.get("installed"),
                 entry.get("enabled"), entry.get("healthy"), entry.get("core")))
        report.append("- installed manifest: version=%s installed=%s enabled=%s healthy=%s core=%s kind=%s"
                      % (entry.get("version"), entry.get("installed"), entry.get("enabled"),
                         entry.get("healthy"), entry.get("core"), entry.get("kind")))
        _write_report(args.report, report)
        if not entry.get("healthy"):
            print("FAIL: installed module is not healthy", file=sys.stderr)
            return 1
        return 0
    finally:
        agent_local.unlink(missing_ok=True)


def _write_report(path: str | None, lines: list[str]) -> None:
    if not path:
        return
    p = pathlib.Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
