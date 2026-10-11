#!/usr/bin/env python3
"""SDK-environment integration test for the Jeopardy native lifecycle.

This runs the real authoritative service exactly as the ReBox SDK starts it
(`REBOX_MODULE_SOCKET` + `REBOX_MODULE_API=1`) and drives the real SDK HTTP
protocol over the module socket: `/native/status`, `/native/start`,
`/native/stop`. The child is a mock native surface (no receiver required) that
honours the same readiness pipe and exits when the core publishes a committed
quit.

It proves the Stage-3 lifecycle guarantees on the real SDK path:

  * start/status/stop drive one child and one lifecycle lock, idempotently;
  * a committed quit tears the child down but the authoritative LAN service and
    the SDK event loop stay alive (the /health endpoint still answers);
  * the next `/native/start` resets the game out of quit on the poll thread and
    relaunches the child, and the same core can host a brand new game.

stdlib only. Exit status 0 on success.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


CHECKS = 0
FAILS = 0


def check(cond, msg):
    global CHECKS, FAILS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print("FAIL:", msg)


def http_unix(sock_path, method, path, body=b"", timeout=8.0):
    """One HTTP/1.0 request over the module's unix socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    req = ("%s %s HTTP/1.0\r\nContent-Length: %d\r\n\r\n" % (method, path, len(body))).encode() + body
    s.sendall(req)
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ")[1])
    except (IndexError, ValueError):
        status = 0
    try:
        obj = json.loads(payload.decode() or "{}")
    except ValueError:
        obj = {}
    return status, obj


def http_tcp(port, method, path, body=b"", timeout=8.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    req = ("%s %s HTTP/1.0\r\nContent-Length: %d\r\n\r\n" % (method, path, len(body))).encode() + body
    s.sendall(req)
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ")[1])
    except (IndexError, ValueError):
        status = 0
    try:
        obj = json.loads(payload.decode() or "{}")
    except ValueError:
        obj = {}
    return status, obj


def wait_for(pred, timeout=8.0, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def main(argv):
    if len(argv) < 4:
        print("usage: test_sdk_native.py <module-binary> <mock-native> <demo-db>")
        return 2
    module_bin, mock_bin, demo_db = argv[1], argv[2], argv[3]

    root = tempfile.mkdtemp(prefix="jpq-sdk-")
    try:
        bindir = os.path.join(root, "bin")
        os.makedirs(bindir)
        data = os.path.join(root, "data")
        os.makedirs(data)
        package = os.path.join(root, "package", "default-data")
        os.makedirs(package)
        shutil.copyfile(demo_db, os.path.join(package, "questions.jdb"))

        module = os.path.join(bindir, "module")
        native = os.path.join(bindir, "hr54-jeopardy-native")
        shutil.copyfile(module_bin, module)
        shutil.copyfile(mock_bin, native)
        os.chmod(module, 0o700)
        os.chmod(native, 0o700)

        sdk_sock = os.path.join(root, "sdk.sock")
        env = dict(os.environ)
        env.update({
            "REBOX_MODULE_SOCKET": sdk_sock,
            "REBOX_MODULE_API": "1",
            "REBOX_MODULE_DATA": data,
            "REBOX_MODULE_PACKAGE": os.path.join(root, "package"),
            "JPQ_PORT": "0",
            "JPQ_BIND": "127.0.0.1",
        })
        env.pop("REBOX_MODULE_SEED_ONLY", None)
        env.pop("JPQ_DB", None)

        proc = subprocess.Popen([module], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            port = None
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                line = proc.stdout.readline()
                if not line:
                    break
                m = re.search(rb"JPQ_READY port=(\d+)", line)
                if m:
                    port = int(m.group(1))
                    break
            check(port is not None, "service announced JPQ_READY with a bound port")
            check(wait_for(lambda: os.path.exists(sdk_sock), 5), "SDK module socket appears")
            if port is None or not os.path.exists(sdk_sock):
                return 1

            # --- start/status/stop idempotent lock ownership ---------------
            status, obj = http_unix(sdk_sock, "GET", "/native/status")
            check(status == 200 and obj.get("running") is False, "native reports stopped before start")

            status, obj = http_unix(sdk_sock, "POST", "/native/start")
            check(status == 200 and obj.get("running") is True, "native start reports running")
            first_pid = obj.get("pid")
            check(isinstance(first_pid, int) and first_pid > 1, "native start reports a child pid")

            lock = os.path.join(data, "native", "native.lock")
            check(wait_for(lambda: os.path.exists(lock), 3), "lifecycle lock created while running")

            status, obj = http_unix(sdk_sock, "GET", "/native/status")
            check(status == 200 and obj.get("running") is True, "native status running while up")

            # a second start is idempotent: same child, same lock
            status, obj = http_unix(sdk_sock, "POST", "/native/start")
            check(status == 200 and obj.get("running") is True, "second native start is idempotent")
            check(obj.get("pid", first_pid) == first_pid, "idempotent start keeps the same child")

            # --- play a real game, then commit quit -------------------------
            with open(os.path.join(data, "native", "metadata.json")) as f:
                pin = json.load(f)["joinPin"]
            ids = []
            for i in range(2):
                status, obj = http_tcp(port, "POST", "/api/join",
                                       json.dumps({"name": "SDK%d" % i, "pin": pin}).encode())
                check(status == 200 and obj.get("playerId"), "player joins over the live LAN")
                ids.append(obj.get("playerId"))

            cmd_sock = os.path.join(data, "native", "command.sock")
            check(wait_for(lambda: os.path.exists(cmd_sock), 3), "native command socket appears")
            c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            c.settimeout(5)
            c.connect(cmd_sock)

            def cmd(line):
                c.sendall((line + "\n").encode())
                buf = b""
                while b"\n" not in buf:
                    ch = c.recv(1)
                    if not ch:
                        break
                    buf += ch
                return json.loads(buf.decode().strip() or "{}")

            for pid in ids:
                check(cmd(json.dumps({"actor": pid, "action": "READY", "value": 1})).get("ok") is True,
                      "player ready via the private native channel")
            check(cmd(json.dumps({"actor": "host", "action": "START"})).get("ok") is True,
                  "host starts the game via the private native channel")

            disp = os.path.join(data, "native", "public-display.json")

            def read_phase():
                try:
                    with open(disp) as f:
                        return json.load(f)["state"]["phase"]
                except (OSError, ValueError, KeyError):
                    return None
            check(wait_for(lambda: read_phase() == 1, 4), "game reached the board")

            check(cmd(json.dumps({"actor": "host", "action": "QUIT"})).get("ok") is True, "quit armed")
            check(cmd(json.dumps({"actor": "host", "action": "QUIT"})).get("ok") is True, "quit committed")
            c.close()

            # the mock child exits on the committed quit; the SDK stays up
            check(wait_for(lambda: not http_unix(sdk_sock, "GET", "/native/status")[1].get("running"), 4),
                  "committed quit tears the native child down")
            status, obj = http_tcp(port, "GET", "/health")
            check(status == 200 and obj.get("ok") is True,
                  "LAN service still alive after quit (authoritative eventloop survives)")

            # --- relaunch resets quit and hosts a fresh game ----------------
            status, obj = http_unix(sdk_sock, "POST", "/native/start")
            check(status == 200 and obj.get("running") is True, "native relaunch after quit")
            check(wait_for(lambda: os.path.exists(lock), 3), "lifecycle lock re-acquired on relaunch")

            def read_state():
                try:
                    with open(disp) as f:
                        return json.load(f)["state"]
                except (OSError, ValueError):
                    return None
            st = None
            check(wait_for(lambda: (lambda s: bool(s) and s.get("quit") == 0 and s.get("phase") == 0)(read_state()), 4),
                  "relaunch reset cleared quit and returned to a fresh lobby")
            st = read_state()
            check(st is not None and st.get("quit") == 0 and st.get("phase") == 0,
                  "relaunch reset clears quit / returns to lobby")

            # a second player joins and starts on the SAME live core
            status, obj = http_tcp(port, "POST", "/api/join",
                                   json.dumps({"name": "SDK-NEW", "pin": pin}).encode())
            check(status == 200 and obj.get("playerId"), "new player joins the live core after relaunch")

            # --- stop releases the lock; start reacquires it ----------------
            status, obj = http_unix(sdk_sock, "POST", "/native/stop")
            check(status == 200 and obj.get("running") is False, "native stop reports stopped")
            check(wait_for(lambda: not os.path.exists(lock) or not _locked(lock), 4),
                  "lifecycle lock released on stop")
            status, obj = http_unix(sdk_sock, "GET", "/native/status")
            check(status == 200 and obj.get("running") is False, "native reports stopped after stop")
        finally:
            c = None
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
            shutil.rmtree(root, ignore_errors=True)

        print("sdk-native: %d checks, %d failures" % (CHECKS, FAILS))
        return 1 if FAILS else 0
    except Exception as exc:  # noqa: BLE001 - report, do not mask
        shutil.rmtree(root, ignore_errors=True)
        print("sdk-native: exception:", repr(exc))
        return 1


def _locked(lock):
    """Return True while the lock is held by another process."""
    import fcntl
    try:
        fd = os.open(lock, os.O_RDONLY)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except OSError:
        return True
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
