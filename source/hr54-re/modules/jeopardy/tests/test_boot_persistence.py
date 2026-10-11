#!/usr/bin/env python3
"""Host acceptance for Jeopardy reboot persistence (boot hook + generic launcher).

This test never touches a live receiver and never reboots anything. It builds an
isolated fake /var, a mock iptables 1.4.7 (which has no `-C`) and, where user
namespaces are available, runs the real persistent launcher in a private mount
namespace to simulate a fresh boot deterministically.

Checks:

* shell syntax of the launcher and the boot hook;
* the generic launcher invokes boot.d hooks fail-isolated and in the background,
  and never gates core/boot on a hook;
* the module boot hook owns exactly one scoped INPUT rule (eth0, 192.168.88.0/24,
  TCP 8135) only when ReBox is enabled, the module manifest is present and the
  schema-1 module state says enabled=true and installed=true;
* disabled / uninstalled / no-ReBox / malformed state fail closed and remove only
  the owned rule, leaving every other rule untouched;
* repeated hook invocations and repeated launcher boots never accumulate
  duplicates;
* a missing iptables or a failing hook cannot break core/boot;
* the launcher never duplicates or kills the core (reboxd) or the stock
  bootstrap, and never waits on a slow hook.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
MODULE = HERE.parent
HOOK = MODULE / "tools" / "jeopardy-network.sh"
LAUNCHER = MODULE.parents[1] / "reboxd" / "rebox-start.sh"
REBOX = "reboxd/rebox-start.sh"  # for messages

SCOPE = "-A INPUT -s 192.168.88.0/24 -i eth0 -p tcp -m tcp --dport 8135 -j ACCEPT"

MOCK_IPTABLES = r'''#!/usr/bin/env python3
"""Mock iptables 1.4.7: -S/-I/-D implemented, -C rejected like the stock box."""
import os, sys

STATE = os.environ["MOCK_IPTABLES_STATE"]


def load():
    try:
        return [l for l in open(STATE).read().splitlines() if l.strip()]
    except FileNotFoundError:
        return []


def save(rules):
    with open(STATE, "w") as f:
        for r in rules:
            f.write(r + "\n")


def fields(tokens):
    d = {}
    longmap = {"--in-interface": "-i", "--source": "-s", "--protocol": "-p",
               "--dport": "--dport", "--jump": "-j"}
    i = 0
    while i < len(tokens):
        t = tokens[i]
        key = longmap.get(t, t)
        if key in ("-A", "-i", "-s", "-p", "--dport", "-j", "-m"):
            if i + 1 >= len(tokens):
                break
            d[key] = tokens[i + 1]
            i += 2
        else:
            i += 1
    return d


def canon(d):
    return "-A INPUT -s %s -i %s -p tcp -m tcp --dport %s -j ACCEPT" % (
        d.get("-s"), d.get("-i"), d.get("--dport"))


args = sys.argv[1:]
if not args:
    sys.exit(2)
op = args[0]
if op == "-C":
    sys.stderr.write("iptables v1.4.7: option `-C' requires an argument\n")
    sys.stderr.write("Try `iptables -h' or 'iptables --help' for more information.\n")
    sys.exit(2)
if op == "--version":
    print("iptables v1.4.7")
    sys.exit(0)
if op == "-S":
    print("-P INPUT ACCEPT")
    for r in load():
        print(r + " ")  # stock 1.4.7 prints a trailing space
    sys.exit(0)
if op == "-I":
    rules = load()
    rules.insert(0, canon(fields(args[1:])))
    save(rules)
    sys.exit(0)
if op == "-D":
    tgt = canon(fields(args[1:]))
    out, removed = [], False
    for r in load():
        if not removed and r.rstrip() == tgt:
            removed = True
            continue
        out.append(r)
    save(out)  # real iptables -D removes exactly one match
    sys.exit(0)
sys.exit(2)
'''

STUB_C = r'''#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *log = getenv("FAKE_START_LOG");
    if (log) {
        FILE *f = fopen(log, "a");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    }
    for (;;) { pause(); }
    return 0;
}
'''

FAILURES: list[str] = []
CHECKS = [0]


def check(name: str, ok: bool, detail: str = "") -> bool:
    CHECKS[0] += 1
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, (" - " + detail) if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)
    return ok


class Sandbox:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.var = root / "var"
        self.mock = root / "mock-iptables"
        self.state = root / "iptables-state.txt"
        self.mock.write_text(MOCK_IPTABLES)
        self.mock.chmod(0o755)
        self.stub = root / "reboxd-stub"
        src = root / "stub.c"
        src.write_text(STUB_C)
        subprocess.run([os.environ.get("CC", "cc"), str(src), "-o", str(self.stub)],
                       check=True, capture_output=True)
        self.start_log = root / "reboxd-starts.log"

    # -- fake persistent tree -------------------------------------------------
    def persist(self) -> pathlib.Path:
        return self.var / "hr54-persist"

    def make_tree(self, *, state: str | None = '{"schema":1,"enabled":true,"installed":true,"version":"0.3.0"}',
                  manifest: bool = True, rebox_enabled: bool = True,
                  hook: bytes | None = None, config: str | None = None,
                  bootstrap: bool = False) -> None:
        shutil.rmtree(self.var, ignore_errors=True)
        base = self.persist()
        for d in ("rebox/bin", "rebox/log", "rebox/module-state", "rebox/modules/jeopardy",
                  "rebox/boot.d", "bin", "native-menu", "opt/hr54/bin"):
            (base / d).mkdir(parents=True, exist_ok=True)
        if rebox_enabled:
            (base / "REBOX_ENABLED").write_text("")
        if state is not None:
            (base / "rebox/module-state/jeopardy.json").write_text(state)
        if manifest:
            (base / "rebox/modules/jeopardy/module.json").write_text('{"schema":1,"id":"jeopardy","version":"0.3.0"}')
        (base / "bin/hr54-play-url").write_text("#!/bin/sh\n")
        dest = base / "rebox/bin/reboxd"
        dest.unlink(missing_ok=True)
        shutil.copy2(self.stub, dest)
        dest.chmod(0o700)
        if hook is not None:
            hp = base / "rebox/boot.d/jeopardy-network.sh"
            hp.write_bytes(hook)
            hp.chmod(0o700)
        if config is not None:
            (base / "rebox/boot.d/jeopardy-network.conf").write_text(config)
        if bootstrap:
            (base / "native-menu/ENABLED").write_text("")
            (base / "native-menu/ACTIVATED").write_text("")
            bs = base / "native-menu/bootstrap.sh"
            bs.write_text("#!/bin/sh\necho $$ >> \"$FAKE_BOOT_LOG\"\nexec sleep 300\n")
            bs.chmod(0o700)

    def hook_bytes(self, *, fail: bool = False) -> bytes:
        body = HOOK.read_bytes()
        if fail:
            body = b"#!/bin/sh\necho 'deliberate hook failure' >&2\nexit 7\n"
        return body

    # -- mock state helpers ---------------------------------------------------
    def rules(self) -> list[str]:
        try:
            return [l.rstrip() for l in self.state.read_text().splitlines() if l.strip()]
        except FileNotFoundError:
            return []

    def seed_rule(self) -> None:
        self.state.write_text(SCOPE + "\n")

    def base_env(self, base: pathlib.Path | None = None) -> dict:
        env = dict(os.environ)
        env["IPTABLES"] = str(self.mock)
        env["MOCK_IPTABLES_STATE"] = str(self.state)
        if base is not None:
            env["HR54_PERSIST_BASE"] = str(base)
        return env

    # -- run the hook directly (no namespace) ---------------------------------
    def run_hook(self, base: pathlib.Path, *, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(["/bin/sh", str(HOOK)], capture_output=True, text=True,
                              env=env or self.base_env(base), timeout=30)

    def stop_all(self) -> None:
        pids: list[int] = []
        for src in (self.start_log, self.root / "bootstrap-starts.log",
                    self.persist() / "rebox/reboxd.pid",
                    self.persist() / "native-menu/bootstrap-launch.pid"):
            try:
                pids += [int(tok) for tok in src.read_text().split()]
            except (FileNotFoundError, ValueError):
                pass
        for pid in pids:
            try:
                os.kill(pid, 9)
            except OSError:
                pass

    # -- run the real launcher in a private mount namespace -------------------
    def run_session(self, body: str, *, env_extra: dict | None = None, timeout: int = 60):
        env = dict(os.environ)
        env["IPTABLES"] = str(self.mock)
        env["MOCK_IPTABLES_STATE"] = str(self.state)
        env["FAKE_START_LOG"] = str(self.start_log)
        env["FAKE_BOOT_LOG"] = str(self.root / "bootstrap-starts.log")
        if env_extra:
            env.update(env_extra)
        script = 'mount --bind "%s" /var && %s' % (self.var, body)
        return subprocess.run(["unshare", "-rm", "/bin/sh", "-c", script],
                              capture_output=True, text=True, env=env, timeout=timeout)

    def run_launcher(self, *, env_extra: dict | None = None, timeout: int = 30):
        return self.run_session('exec /bin/sh "%s"' % LAUNCHER, env_extra=env_extra,
                                timeout=timeout)

    def hook_log(self) -> str:
        p = self.persist() / "rebox/log/boot-hook-jeopardy-network.log"
        try:
            return p.read_text()
        except FileNotFoundError:
            return ""

    def wait_hook(self, count: int = 1, timeout: float = 8.0) -> None:
        # The launcher writes "boot hook <name> ok|failed ..." only after the
        # hook process has fully exited, so this also serialises scenarios.
        end = time.time() + timeout
        while time.time() < end:
            if self.hook_log().count("boot hook ") >= count:
                return
            time.sleep(0.05)

    def wait_lines(self, path: pathlib.Path, n: int, timeout: float = 8.0) -> None:
        end = time.time() + timeout
        while time.time() < end:
            try:
                if len(path.read_text().split()) >= n:
                    return
            except FileNotFoundError:
                pass
            time.sleep(0.05)

    def alive(self, pid: int) -> bool:
        return pid > 0 and pathlib.Path("/proc/%d" % pid).exists()


def syntax_checks(sb: Sandbox) -> None:
    for label, path in (("launcher", LAUNCHER), ("hook", HOOK)):
        r = subprocess.run(["/bin/sh", "-n", str(path)], capture_output=True, text=True)
        check("shell syntax: %s (%s)" % (label, path.name), r.returncode == 0, r.stderr.strip())
    src = LAUNCHER.read_text()
    check("launcher: additive boot.d hook block", "bootd=$runtime/boot.d" in src)
    check("launcher: hooks run in the background", ') >> "$runtime/log/boot-hook-' in src and "&" in src)
    check("launcher: rejects symlinked hooks/dir", '[ ! -L "$hook" ]' in src and '[ ! -L "$bootd" ]' in src)
    check("launcher: hook block follows the ReBox marker check",
          src.index("REBOX_ENABLED") < src.index("bootd=$runtime/boot.d"))
    check("launcher: hook block precedes module bind",
          src.index("bootd=$runtime/boot.d") < src.index('exec "$runtime/bin/reboxd"'))
    hook_src = HOOK.read_text()
    check("hook: exact match is busybox-1.16.1 safe (no grep -x / awk)",
          "-Fxc" not in hook_src and "grep -x" not in hook_src and "awk" not in hook_src
          and "while read -r line" in hook_src)
    check("hook: owns a bound and does not touch other INPUT rules",
          'iptables" -D INPUT $spec' in hook_src and 'iptables" -I INPUT 1 $spec' in hook_src)
    check("hook: fail-closed schema-1 state gate", '"schema":1,"enabled":true,"installed":true' in hook_src)


def hook_state_checks(sb: Sandbox) -> None:
    base = sb.persist()

    # enabled + installed + manifest + marker -> exactly one owned rule, repeatedly.
    sb.make_tree()
    for i in range(3):
        r = sb.run_hook(base)
        check("hook enabled run %d ok" % (i + 1),
              r.returncode == 0 and ("added" in r.stdout or "present" in r.stdout))
    check("hook enabled: exactly one scoped rule after repeats", sb.rules() == [SCOPE],
          "rules=%r" % sb.rules())
    check("hook enabled: rule is exactly the private scoped spec", sb.rules() == [SCOPE])

    # Starting from a duplicate never accumulates further and normalises to one.
    sb.state.write_text("\n".join([SCOPE, SCOPE, SCOPE]) + "\n")
    sb.run_hook(base)
    check("hook enabled: duplicates normalised to one", sb.rules() == [SCOPE], "rules=%r" % sb.rules())

    # disabled -> owned rule removed, nothing else kept.
    (base / "rebox/module-state/jeopardy.json").write_text('{"schema":1,"enabled":false,"installed":true,"version":"0.3.0"}')
    sb.run_hook(base)
    check("hook disabled: owned rule removed", sb.rules() == [], "rules=%r" % sb.rules())

    # installed=false -> removed.
    sb.seed_rule()
    (base / "rebox/module-state/jeopardy.json").write_text('{"schema":1,"enabled":true,"installed":false,"version":"0.3.0"}')
    sb.run_hook(base)
    check("hook uninstalled (state): owned rule removed", sb.rules() == [])

    # module manifest absent -> removed.
    sb.seed_rule()
    (base / "rebox/module-state/jeopardy.json").write_text('{"schema":1,"enabled":true,"installed":true,"version":"0.3.0"}')
    (base / "rebox/modules/jeopardy/module.json").unlink()
    sb.run_hook(base)
    check("hook module absent: owned rule removed", sb.rules() == [])

    # no ReBox marker -> removed.
    (base / "rebox/modules/jeopardy/module.json").write_text('{"schema":1}')
    sb.seed_rule()
    (base / "REBOX_ENABLED").unlink()
    sb.run_hook(base)
    check("hook no ReBox marker: owned rule removed", sb.rules() == [])

    # malformed / wrong-schema state fails closed.
    (base / "REBOX_ENABLED").write_text("")
    for label, bad in [
        ("truncated", '{"schema":1,"enabled":true,"installed":true'),
        ("wrong schema", '{"schema":2,"enabled":true,"installed":true,"version":"0.3.0"}'),
        ("missing enabled", '{"schema":1,"installed":true,"version":"0.3.0"}'),
        ("non-bool enabled", '{"schema":1,"enabled":1,"installed":true,"version":"0.3.0"}'),
        ("extra top-level", '{"schema":1,"enabled":true,"installed":true,"version":"0.3.0","x":1}'),
    ]:
        sb.seed_rule()
        (base / "rebox/module-state/jeopardy.json").write_text(bad)
        sb.run_hook(base)
        check("hook malformed state (%s) fails closed" % label, sb.rules() == [])

    # config scope is honoured and no broader rule is created.
    sb.make_tree(config="INTERFACE=eth0\nLAN=192.168.88.0/24\nPORT=8135\n")
    sb.run_hook(base)
    check("hook config scope: exact scoped rule", sb.rules() == [SCOPE])

    # a missing iptables is a harmless no-op (fail isolated).
    sb.make_tree()
    sb.state.write_text("")
    env = sb.base_env(base)
    env["IPTABLES"] = str(sb.root / "does-not-exist")
    r = sb.run_hook(base, env=env)
    check("hook missing iptables: no crash and no rule", r.returncode == 0 and sb.rules() == [])

    # an unexpected config value is ignored, never widens the scope.
    sb.make_tree(config="INTERFACE=eth0\nLAN=0.0.0.0/0\nPORT=8135\n")
    sb.state.write_text("")
    sb.run_hook(base)
    check("hook config refuses broad LAN 0.0.0.0/0", sb.rules() == [SCOPE], "rules=%r" % sb.rules())


def launcher_checks(sb: Sandbox) -> None:
    if shutil.which("unshare") is None:
        check("launcher namespace tests require unshare", False, "unshare not available")
        return
    probe = subprocess.run(["unshare", "-rm", "true"], capture_output=True)
    if probe.returncode != 0:
        check("launcher namespace tests require a usable user namespace", False,
              probe.stderr.decode(errors="replace").strip())
        return

    hook = sb.hook_bytes()

    def reset(**kw) -> None:
        sb.stop_all()
        sb.make_tree(**kw)
        sb.start_log.write_text("")
        (sb.root / "bootstrap-starts.log").write_text("")

    def starts() -> list[int]:
        return [int(x) for x in (sb.start_log.read_text().split() if sb.start_log.exists() else [])]

    def boots() -> list[int]:
        p = sb.root / "bootstrap-starts.log"
        return [int(x) for x in (p.read_text().split() if p.exists() else [])]

    # Fresh boot followed by a warm re-run, in one mount namespace (a real box
    # has a single namespace). Core + stock bootstrap start once; the warm
    # re-run detects the running core and never restarts or kills it; the hook
    # adds exactly one scoped rule and never accumulates.
    reset(hook=hook, bootstrap=True)
    sb.state.write_text("")
    body = ('/bin/sh "%s"; sleep 0.4; rm -f /var/hr54-persist/native-menu/ACTIVATED; '
            '/bin/sh "%s"' % (LAUNCHER, LAUNCHER))
    r = sb.run_session(body)
    sb.wait_hook(2)
    sb.wait_lines(sb.start_log, 1)
    sb.wait_lines(sb.root / "bootstrap-starts.log", 1)
    check("launcher fresh boot: exit 0", r.returncode == 0, r.stderr)
    check("launcher fresh boot: core started exactly once", len(starts()) == 1, "starts=%r" % starts())
    check("launcher fresh boot: bootstrap started exactly once", len(boots()) == 1, "boots=%r" % boots())
    check("launcher fresh boot: hook added exactly one scoped rule", sb.rules() == [SCOPE],
          "rules=%r" % sb.rules())
    core_pid = starts()[0] if starts() else 0
    check("launcher warm re-run: core not duplicated", starts() == [core_pid], "starts=%r" % starts())
    check("launcher warm re-run: core left running (not killed)", sb.alive(core_pid))
    check("launcher warm re-run: still exactly one scoped rule", sb.rules() == [SCOPE])

    # A failing hook must not gate or crash core/boot.
    reset(hook=sb.hook_bytes(fail=True), bootstrap=True)
    sb.state.write_text(SCOPE + "\n")
    r = sb.run_launcher()
    sb.wait_hook()
    sb.wait_lines(sb.start_log, 1)
    sb.wait_lines(sb.root / "bootstrap-starts.log", 1)
    check("launcher failing hook: launcher still exits 0", r.returncode == 0, r.stderr)
    check("launcher failing hook: core still started once", len(starts()) == 1, "starts=%r" % starts())
    check("launcher failing hook: bootstrap still started once", len(boots()) == 1, "boots=%r" % boots())
    check("launcher failing hook: failure recorded in the hook log", "failed" in sb.hook_log())
    check("launcher failing hook: existing rule untouched", sb.rules() == [SCOPE],
          "rules=%r" % sb.rules())

    # A missing iptables must not gate or crash core/boot either.
    reset(hook=hook, bootstrap=True)
    sb.state.write_text("")
    r = sb.run_launcher(env_extra={"IPTABLES": str(sb.root / "does-not-exist")})
    sb.wait_hook()
    sb.wait_lines(sb.start_log, 1)
    sb.wait_lines(sb.root / "bootstrap-starts.log", 1)
    check("launcher missing iptables: launcher still exits 0", r.returncode == 0, r.stderr)
    check("launcher missing iptables: core still started once", len(starts()) == 1, "starts=%r" % starts())
    check("launcher missing iptables: bootstrap still started once", len(boots()) == 1, "boots=%r" % boots())

    # No REBOX_ENABLED: the launcher exits before any hook and never runs core.
    reset(hook=hook, rebox_enabled=False)
    sb.state.write_text(SCOPE + "\n")
    r = sb.run_launcher()
    check("launcher no ReBox: exits 0 without starting core", r.returncode == 0 and starts() == [])
    check("launcher no ReBox: hook did not run", "jeopardy-network:" not in sb.hook_log())
    check("launcher no ReBox: pre-existing rule untouched by launcher", sb.rules() == [SCOPE])

    sb.stop_all()


def main() -> int:
    print("== boot persistence host acceptance ==")
    print("launcher: %s" % LAUNCHER)
    print("hook:     %s" % HOOK)
    with tempfile.TemporaryDirectory(prefix="jpq-persist-") as td:
        sb = Sandbox(pathlib.Path(td))
        syntax_checks(sb)
        hook_state_checks(sb)
        launcher_checks(sb)
    print("")
    print("checks: %d, failures: %d" % (CHECKS[0], len(FAILURES)))
    if FAILURES:
        for name in FAILURES:
            print("  FAILED: %s" % name)
        return 1
    print("BOOT PERSISTENCE: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
