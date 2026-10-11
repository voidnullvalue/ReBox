#!/usr/bin/env python3
"""Stage 2 network integration tests for the Jeopardy service.

Uses only the Python standard library. Spawns the real authoritative host
executable and drives it with raw WebSocket clients (tests/wsclient.py),
covering joins/capacity, buzzer races, authorization, replay/staleness, framing
bounds, privacy, reconnect/restart and a full network game.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WSClient, WSError, http_get, http_post  # noqa: E402

LOBBY, BOARD, CLUE, DD_WAGER, DD_CLUE, FINAL_WAGER, FINAL_CLUE, FINAL_JUDGE, RESULTS, TIEBREAK, REVEAL = range(11)
TIMER_NONE, TIMER_READ, TIMER_BUZZ, TIMER_ANSWER = 0, 1, 2, 3

CHECKS = 0
FAILS = 0


def check(cond, msg):
    global CHECKS, FAILS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print(f"FAIL: {msg}")


class Service:
    def __init__(self, exe, data, db, port=0, env=None):
        self.data = data
        self.exe = exe
        self.db = db
        e = os.environ.copy()
        e.update({"JPQ_DATA": data, "JPQ_DB": db, "JPQ_PORT": str(port), "JPQ_BIND": "127.0.0.1"})
        if env:
            e.update(env)
        self.proc = subprocess.Popen([exe], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        line = self.proc.stdout.readline()
        if "JPQ_READY" not in line:
            raise RuntimeError("service did not start: " + line + self.proc.stderr.read())
        self.port = int(line.split("port=")[1].split()[0])
        self.metadata = self._read_metadata()

    def _read_metadata(self):
        with open(os.path.join(self.data, "native", "metadata.json")) as f:
            return json.load(f)

    def public_display(self):
        with open(os.path.join(self.data, "native", "public-display.json")) as f:
            return json.load(f)

    def stop(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


class Player:
    def __init__(self, svc: Service, name=None, color=None, pin=None, token=None):
        self.svc = svc
        self.ws = WSClient("127.0.0.1", svc.port)
        self.state = None
        self.gs = None
        self.seq = 0
        self.token = token
        self.player_id = None
        self.errors = []
        if token:
            self.ws.send_json({"type": "hello", "token": token})
        elif name is not None:
            self.join(name, color, pin)
        self._wait_state()

    # -- auth ----------------------------------------------------------
    def join(self, name, color, pin):
        st, js, txt = http_post("127.0.0.1", self.svc.port, "/api/join", {"name": name, "color": color, "pin": pin})
        if st != 200:
            raise RuntimeError(f"join failed {st} {txt}")
        self.token = js["token"]
        self.player_id = js["playerId"]
        self.ws.send_json({"type": "hello", "token": self.token})

    @staticmethod
    def spectate(svc):
        p = Player.__new__(Player)
        p.svc = svc
        p.ws = WSClient("127.0.0.1", svc.port)
        p.state = None
        p.gs = None
        p.seq = 0
        p.token = None
        p.player_id = None
        p.errors = []
        p.ws.send_json({"type": "spectate"})
        p._wait_state()
        return p

    @staticmethod
    def host(svc, pin):
        p = Player.__new__(Player)
        p.svc = svc
        p.ws = WSClient("127.0.0.1", svc.port)
        p.state = None
        p.gs = None
        p.seq = 0
        st, js, txt = http_post("127.0.0.1", svc.port, "/api/host", {"pin": pin})
        if st != 200:
            raise RuntimeError(f"host auth failed {st} {txt}")
        p.token = js["token"]
        p.player_id = None
        p.errors = []
        p.ws.send_json({"type": "hello", "token": p.token})
        p._wait_state()
        return p

    # -- messaging -----------------------------------------------------
    def _ingest(self, m):
        if m.get("type") == "state":
            self.state = m
            self.gs = m["state"]
        elif m.get("type") == "error":
            self.errors.append(m)

    def _wait_state(self, timeout=6.0):
        self.state = self.ws.wait_state(timeout=timeout)
        self.gs = self.state["state"]
        self.seq = max(self.seq, self.state.get("seq", 0))
        return self.state

    def wait(self, pred, timeout=6.0):
        m = self.ws.wait_state(pred=lambda w: pred(w.get("state") or {}), timeout=timeout)
        if m.get("type") == "state":
            self._ingest(m)
        return self.gs

    def wait_phase(self, phases, timeout=6.0):
        if isinstance(phases, int):
            phases = (phases,)
        return self.wait(lambda gs: gs.get("phase") in phases, timeout)

    def drain(self, budget=0.15):
        end = time.time() + budget
        while time.time() < end:
            m = self.ws.try_recv_json(timeout=0.03)
            if not m:
                break
            self._ingest(m)

    def cmd(self, action, **fields):
        self.seq += 1
        if self.state:
            self.seq = max(self.seq, self.state.get("seq", 0) + 1)
        o = {"type": "cmd", "seq": self.seq, "action": action}
        if self.state:
            o["epoch"] = self.state.get("epoch")
            o["window"] = self.state.get("window")
        o.update(fields)
        self.ws.send_json(o)
        return self.seq

    def expect_error(self, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.ws.try_recv_json(timeout=max(0.05, end - time.time()))
            if not m:
                break
            if m.get("type") == "error":
                return m
            self._ingest(m)
        return None

    def close(self):
        self.ws.close()

    def players(self):
        return self.gs["players"] if self.gs else []

    def me(self):
        for p in self.players():
            if p["id"] == self.player_id:
                return p
        return None


# --------------------------------------------------------------------------
# scenario helpers
# --------------------------------------------------------------------------

def join_players(svc, names, pin):
    players = []
    for i, n in enumerate(names):
        players.append(Player(svc, name=n, color=None, pin=pin))
    for p in players:
        p.cmd("READY", value=1)
        p.wait(lambda gs: any(x["id"] == p.player_id and x["ready"] for x in gs["players"]))
    return players


def configure_fast(host):
    host.cmd("CONFIG", config={
        "read_ms": 20, "buzz_ms": 4000, "answer_ms": 3000,
        "dd_wager_ms": 2000, "dd_answer_ms": 3000,
        "final_wager_ms": 2000, "final_answer_ms": 3000, "tiebreak_answer_ms": 3000,
    })
    time.sleep(0.15)
    host.drain()


def find_cell(gs, want_dd):
    board = gs["board"]
    for col, cat in enumerate(board["categories"]):
        for row, cell in enumerate(cat["clues"]):
            if cell["used"]:
                continue
            if bool(cell["daily_double"]) == bool(want_dd):
                return col * 5 + row
    return None


def open_buzz_window(host, clients):
    """Wait until the current clue's buzz window is open for everyone."""
    host.wait(lambda gs: gs["phase"] in (CLUE, TIEBREAK) and gs["timer"]["kind"] == 2, timeout=4)
    for c in clients:
        c.drain()


def buzz_and_judge(host, p, correct=True):
    p.drain()
    p.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    host.cmd("JUDGE", target=p.player_id, value=1 if correct else -1)
    host.wait_phase(REVEAL, timeout=3)
    host.cmd("ADVANCE")
    host.wait_phase((BOARD, DD_WAGER, FINAL_WAGER, RESULTS, TIEBREAK), timeout=3)


# --------------------------------------------------------------------------
# tests
# --------------------------------------------------------------------------

def test_http_and_privacy_of_pins(svc):
    st, js, txt = http_get("127.0.0.1", svc.port, "/health")
    check(st == 200 and js and js.get("ok"), "health ok")
    st, js, txt = http_get("127.0.0.1", svc.port, "/")
    check(st == 200 and "<html" in txt.lower(), "index served")
    check("http://" not in txt and "https://" not in txt, "webpage has no external URLs")
    st, js, txt = http_get("127.0.0.1", svc.port, "/api/state")
    check(st == 200 and js and js.get("role") == "public", "public state")
    pin = svc.metadata["joinPin"]
    check(pin not in txt.replace(svc.metadata["metadataUrl"], ""), "join pin not in public state")
    check(svc.metadata["hostPin"] not in txt, "host pin not in public state")
    # native metadata/display are never exposed over LAN
    for path in ("/native/metadata.json", "/native/public-display.json", "/native/command.sock"):
        st, js, txt = http_get("127.0.0.1", svc.port, path)
        check(st == 404, f"private path {path} is 404")
    # mode: metadata file is private
    mode = os.stat(os.path.join(svc.data, "native", "metadata.json")).st_mode & 0o777
    check(mode == 0o600, "metadata file is 0600")


def test_join_capacity_and_validation(svc):
    pin = svc.metadata["joinPin"]
    # bad pin rejected
    st, js, _ = http_post("127.0.0.1", svc.port, "/api/join", {"name": "X", "color": "cobalt", "pin": "0000" if pin != "0000" else "9999"})
    check(st == 401, "bad join pin rejected")
    names = ["Alice", "Bob", "Carol", "Dave", "Erin", "Frank"]
    players = join_players(svc, names, pin)
    check(len(players[0].players()) == 6, "six players joined")
    # duplicate name/color rejected
    st, js, _ = http_post("127.0.0.1", svc.port, "/api/join", {"name": "Alice", "color": "jade", "pin": pin})
    check(st == 409, "duplicate name rejected")
    st, js, _ = http_post("127.0.0.1", svc.port, "/api/join", {"name": "Gina", "color": "cobalt", "pin": pin})
    check(st == 409, "duplicate color rejected")
    st, js, _ = http_post("127.0.0.1", svc.port, "/api/join", {"name": "Gina", "color": "jade", "pin": pin})
    check(st == 409, "seventh player rejected (capacity)")
    for p in players:
        p.close()


def test_buzz_race_and_auth(svc):
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Ann", "Ben"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    a.drain(); b.drain()

    # unauthorized: player tries host actions / score / judge
    a.cmd("START")
    e = a.expect_error()
    check(e is not None and "host" in (e.get("error", "")), "player cannot START")
    a.cmd("SCORE", target=a.player_id, value=100)
    e = a.expect_error()
    check(e is not None and "host" in (e.get("error", "")), "player cannot override score")
    # spoofed actor field is ignored (actor always derived from token)
    a.seq += 1
    a.ws.send_json({"type": "cmd", "seq": a.seq, "epoch": a.state["epoch"], "window": a.state["window"],
                    "action": "START", "actor": "host", "seq": a.seq})
    e = a.expect_error()
    check(e is not None and "host" in (e.get("error", "")), "spoofed actor is ignored")

    # open a normal clue and race the buzzer
    cell = find_cell(a.gs, False)
    host.cmd("SELECT", target=cell)
    open_buzz_window(host, [a, b])
    a.drain(); b.drain()
    # both send immediately using the same window
    a.cmd("BUZZ")
    b.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    winner_index = host.gs["buzzed"]
    winner = a if host.players()[winner_index]["id"] == a.player_id else b
    loser = b if winner is a else a
    check(winner_index in (0, 1), "a valid first buzz won")
    le = loser.expect_error(timeout=2)
    check(le is not None, "loser of the buzz race got an error")
    # winner is judged wrong -> locked out (no typed submit for a normal clue)
    host.cmd("JUDGE", target=winner.player_id, value=-1)
    host.wait(lambda gs: any(x["id"] == winner.player_id and x["excluded"] for x in gs["players"]), timeout=3)
    check(any(x["id"] == winner.player_id and x["excluded"] for x in host.players()), "wrong player excluded")
    # rebuzz: the excluded player is locked out, the other may buzz
    loser.wait(lambda gs: gs["timer"]["kind"] == 2 and gs["phase"] in (CLUE,), timeout=3)
    loser.drain()
    loser.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    check(host.players()[host.gs["buzzed"]]["id"] == loser.player_id, "rebuzz allowed after exclusion")
    host.cmd("JUDGE", target=loser.player_id, value=1)
    host.wait_phase(REVEAL, timeout=3)
    host.cmd("ADVANCE")
    host.wait_phase(BOARD, timeout=3)
    a.close(); b.close(); host.close()


def test_replay_and_stale(svc):
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Cid", "Dee"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    cell = find_cell(a.gs, False)
    host.cmd("SELECT", target=cell)
    open_buzz_window(host, [a, b])
    a.drain()
    seq = a.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    # replay: resend the same sequence
    a.ws.send_json({"type": "cmd", "seq": seq, "epoch": a.state["epoch"], "window": a.state["window"], "action": "BUZZ"})
    e = a.expect_error()
    check(e is not None and "replay" in (e.get("error", "")), "replayed sequence rejected")
    # stale clue window
    a.cmd("SUBMIT", text="x", window=max(0, a.state["window"] - 5))
    e = a.expect_error()
    check(e is not None and ("clue" in (e.get("error", "")) or "stale" in (e.get("error", ""))), "stale window rejected")
    a.close(); b.close(); host.close()


def expect_close(client, timeout=2.0):
    """Read frames until the server closes; returns (closed, code)."""
    client.sock.settimeout(timeout)
    code = b""
    for _ in range(12):
        try:
            op, payload = client.recv_frame()
        except (WSError, OSError, socket.timeout, TimeoutError):
            return True, code
        if op == 0x8:
            code = payload[:2]
            return True, code
    return False, code


def test_framing_bounds(svc):
    # unmasked client frame -> protocol close
    c = WSClient("127.0.0.1", svc.port)
    c.send_frame(0x1, b"{}", mask=False)
    closed, code = expect_close(c)
    check(closed, "unmasked frame closes connection")
    check(code == b"\x03\xea", "unmasked frame closes with 1002")
    c.close()

    # oversized declared length -> 1009
    c = WSClient("127.0.0.1", svc.port)
    import struct as _s
    c.send_raw(b"\x81\xff" + _s.pack(">Q", 200000) + b"\x00\x00\x00\x00")
    closed, code = expect_close(c)
    check(closed, "oversized frame closes connection")
    check(code == b"\x03\xf1", "oversized frame closes with 1009")
    c.close()

    # malformed HTTP -> 400
    s = socket.create_connection(("127.0.0.1", svc.port), timeout=3)
    s.sendall(b"garbage\r\n\r\n")
    data = s.recv(200)
    check(b"400" in data or b"404" in data, "garbage request rejected")
    s.close()

    # slow client cannot block others
    slow = socket.create_connection(("127.0.0.1", svc.port), timeout=3)
    slow.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")  # never finish
    time.sleep(0.1)
    st, js, _ = http_get("127.0.0.1", svc.port, "/health")
    check(st == 200, "loop healthy while a slow client is stalled")
    slow.close()


def test_connection_cap(svc):
    # dedicated service: at most 16 simultaneous websocket connections
    clients = []
    for _ in range(17):
        try:
            clients.append(WSClient("127.0.0.1", svc.port, timeout=2))
        except (WSError, OSError, socket.timeout, TimeoutError):
            break
    check(len(clients) <= 16, "at most 16 websocket connections")
    check(len(clients) < 17, "17th websocket connection refused (bounded connections)")
    for c in clients:
        c.close()
    time.sleep(0.3)
    st, js, _ = http_get("127.0.0.1", svc.port, "/health")
    check(st == 200, "loop healthy after cap test")


def test_private_data(svc):
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Eve", "Fox"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    spec = Player.spectate(svc)

    # daily double: the selector wagers and answers privately
    dd = find_cell(host.gs, True)
    check(dd is not None, "daily double available")
    host.cmd("SELECT", target=dd)
    host.wait_phase(DD_WAGER)
    sel_idx = host.gs["selector"]
    selector = a if host.players()[sel_idx]["id"] == a.player_id else b
    other = b if selector is a else a
    selector.wait(lambda gs: gs["phase"] == DD_WAGER, timeout=3)
    selector.cmd("WAGER", value=100)
    host.wait_phase(DD_CLUE)
    other.wait(lambda gs: gs["phase"] == DD_CLUE, timeout=3)
    spec.wait(lambda gs: gs["phase"] == DD_CLUE, timeout=3)
    for o in (other, spec):
        me = [x for x in o.players() if x["id"] == selector.player_id][0]
        check(me["wager"] is None, f"wager private for {o.state['role']}")
    selector.wait(lambda gs: gs["phase"] == DD_CLUE, timeout=3)
    selector.cmd("SUBMIT", text="SECRET-ANSWER")
    host.wait(lambda gs: any(x["id"] == selector.player_id and x["answered"] for x in gs["players"]), timeout=3)
    other.wait(lambda gs: any(x["id"] == selector.player_id and x["answered"] for x in gs["players"]), timeout=3)
    spec.wait(lambda gs: any(x["id"] == selector.player_id and x["answered"] for x in gs["players"]), timeout=3)
    for o in (other, spec):
        me = [x for x in o.players() if x["id"] == selector.player_id][0]
        check(me["answer"] is None, f"answer private for {o.state['role']}")
        check("SECRET-ANSWER" not in json.dumps(o.gs), f"private answer hidden from {o.state['role']}")
    check("SECRET-ANSWER" in json.dumps(host.gs), "host sees the private answer")
    check(spec.gs["clue"]["response"] is None, "spectator cannot see the clue response before reveal")
    for cat in spec.gs["board"]["categories"]:
        for cell in cat["clues"]:
            check(cell["daily_double"] == 0, "spectator cannot see daily double flags")
            break
    a.close(); b.close(); host.close(); spec.close()


def test_reconnect_and_restart(svc):
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Gus", "Hal"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    host.cmd("SELECT", target=find_cell(a.gs, False))
    open_buzz_window(host, [a, b])
    a.drain(); a.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    host.cmd("JUDGE", target=a.player_id, value=1)
    host.wait_phase(REVEAL)
    score = next(x["score"] for x in host.players() if x["id"] == a.player_id)
    check(score > 0, "player scored before reconnect")
    token = a.token
    a.close()
    time.sleep(0.2)
    a2 = Player(svc, token=token)
    pid = a2.state["you"]["playerId"]
    check(pid == a.player_id, "reconnect preserves player identity")
    score2 = next(x["score"] for x in a2.players() if x["id"] == pid)
    check(score2 == score, "reconnect preserves score")

    # host reconnect
    htoken = host.token
    host.close()
    time.sleep(0.2)
    host2 = Player(svc, token=htoken)
    check(host2.state["role"] == "host", "host reconnect")

    # service restart preserving session
    data = svc.data
    host.close()
    a2.close(); b.close()
    svc.stop()
    time.sleep(0.2)
    svc2 = Service(svc.exe, data, svc.db)
    a3 = Player(svc2, token=token)
    check(a3.state["role"] == "player", "player session survives restart")
    pid3 = a3.state["you"]["playerId"]
    check(pid3 == a.player_id, "identity survives restart")
    score3 = next(x["score"] for x in a3.players() if x["id"] == pid3)
    check(score3 == score, "score survives restart")
    a3.close()
    return svc2


def test_full_game(svc):
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Ivy", "Jon"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    a.drain(); b.drain()

    # negative: a wrong answer drops the score
    cell = find_cell(host.gs, False)
    host.cmd("SELECT", target=cell)
    open_buzz_window(host, [a, b])
    a.drain(); a.cmd("BUZZ")
    host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
    host.cmd("JUDGE", target=a.player_id, value=-1)
    host.wait_phase(REVEAL)
    neg = next(x["score"] for x in host.players() if x["id"] == a.player_id)
    check(neg < 0, "negative score after a wrong answer")
    host.cmd("ADVANCE")
    host.wait_phase(BOARD)

    # daily double: selector wagers and answers privately
    dd = find_cell(host.gs, True)
    check(dd is not None, "daily double present on the single board")
    if dd is not None:
        host.cmd("SELECT", target=dd)
        host.wait_phase(DD_WAGER)
        selector_idx = host.gs["selector"]
        selector = a if host.players()[selector_idx]["id"] == a.player_id else b
        selector.drain()
        before = next(x["score"] for x in host.players() if x["id"] == selector.player_id)
        selector.cmd("WAGER", value=100)
        host.wait_phase(DD_CLUE)
        selector.wait(lambda gs: gs["phase"] == DD_CLUE, timeout=3)
        selector.cmd("SUBMIT", text="ddanswer")
        host.wait(lambda gs: gs["phase"] in (REVEAL,) or any(x["answered"] for x in gs["players"]), timeout=3)
        host.cmd("JUDGE", target=selector.player_id, value=1)
        host.wait_phase(REVEAL)
        host.cmd("ADVANCE")
        host.wait_phase(BOARD)
        after = next(x["score"] for x in host.players() if x["id"] == selector.player_id)
        check(after == before + 100, "daily double wager applied")

    # blow through the rest of Jeopardy + Double Jeopardy
    guard = 0
    while host.gs["phase"] == BOARD and guard < 80:
        guard += 1
        c = find_cell(host.gs, False)
        if c is None:
            c = find_cell(host.gs, True)
        if c is None:
            break
        host.cmd("SELECT", target=c)
        host.wait_phase((CLUE, DD_WAGER))
        if host.gs["phase"] == DD_WAGER:
            host.cmd("ADVANCE")
            host.wait_phase(DD_CLUE)
        host.cmd("REVEAL")
        host.wait_phase(REVEAL)
        host.cmd("ADVANCE")
        host.wait_phase((BOARD, FINAL_WAGER, RESULTS))
        a.drain(); b.drain()
    check(guard < 80, "board traversal completed")
    check(host.gs["phase"] in (FINAL_WAGER, RESULTS), "reached Final")

    if host.gs["phase"] == FINAL_WAGER:
        for p in (a, b):
            p.wait(lambda gs: gs["phase"] == FINAL_WAGER, timeout=3)
            me = p.me()
            if me and me.get("wager_min") is not None:
                p.cmd("WAGER", value=0)
        host.wait_phase(FINAL_CLUE)
        for p in (a, b):
            p.wait(lambda gs: gs["phase"] == FINAL_CLUE, timeout=3)
            me = p.me()
            if me and me.get("eligible") and not me.get("answered"):
                p.cmd("SUBMIT", text="final")
        host.wait_phase(FINAL_JUDGE)
        while host.gs["phase"] == FINAL_JUDGE:
            idx = host.gs["judge_cursor"]
            if idx is None or idx >= len(host.gs["players"]):
                break
            target = host.gs["players"][idx]["id"]
            host.cmd("JUDGE", target=target, value=1)
            host.wait(lambda gs: gs["phase"] == RESULTS or gs["judge_cursor"] != idx, timeout=3)
        check(host.gs["phase"] == RESULTS, "Final judged to results")

    check(host.gs["phase"] == RESULTS, "game reached results")

    # ties: equalize the top two players, then run the sudden-death tiebreak
    scores = [p["score"] for p in host.players() if p["active"]]
    top = max(scores)
    leaders = [p for p in host.players() if p["active"] and p["score"] == top]
    if len(leaders) < 2:
        # add to the runner-up to force a tie
        others = [p for p in host.players() if p["active"] and p["score"] != top]
        if others:
            runner = max(others, key=lambda p: p["score"])
            host.cmd("SCORE", target=runner["id"], value=top - runner["score"])
            host.drain()
    host.cmd("TIEBREAK")
    ok = host.wait_phase(TIEBREAK, timeout=3)
    check(host.gs["phase"] == TIEBREAK, "tiebreak started")
    if host.gs["phase"] == TIEBREAK:
        open_buzz_window(host, [a, b])
        eligible = [i for i, p in enumerate(host.players()) if p["eligible"] and p["active"]]
        check(len(eligible) >= 2, "two players eligible for the tiebreak")
        chooser = a if a.player_id in [host.players()[i]["id"] for i in eligible] else b
        chooser.drain(); chooser.cmd("BUZZ")
        host.wait(lambda gs: gs["buzzed"] is not None and gs["buzzed"] >= 0, timeout=3)
        host.cmd("JUDGE", target=chooser.player_id, value=1)
        host.wait_phase(RESULTS, timeout=3)
        check(host.gs["phase"] == RESULTS, "tiebreak resolved to results")
    a.close(); b.close(); host.close()


def test_native_channel(svc):
    """The private native control socket and redacted public display."""
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    join_players(svc, ["Kim", "Lee"], pin)
    sock = svc.metadata["commandSocket"]
    check(os.path.exists(sock), "native command socket exists")
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(3)
    c.connect(sock)
    c.sendall(json.dumps({"actor": "host", "action": "START"}).encode() + b"\n")
    time.sleep(0.3)
    host.wait_phase(BOARD, timeout=3)
    check(host.gs["phase"] == BOARD, "native host command started the game")
    # an unknown actor id is ignored, never mistaken for a seat or the host
    c.sendall(json.dumps({"actor": 999999, "action": "ADVANCE"}).encode() + b"\n")
    time.sleep(0.2)
    check(host.gs["phase"] == BOARD, "unknown native actor ignored")
    c.sendall(b"not json at all\n")
    time.sleep(0.1)
    check(host.gs["phase"] == BOARD, "malformed native line ignored")
    c.close()
    # public display is a redacted snapshot
    pub = svc.public_display()["state"]
    check(pub["phase"] == BOARD, "public display tracks the board phase")
    for cat in pub["board"]["categories"]:
        for cell in cat["clues"]:
            check(cell["daily_double"] == 0, "public display hides daily doubles")
            break
    host.close()


def test_ws_protocol_frames(svc):
    """Binary data frames, invalid UTF-8 text and reserved opcodes are handled."""
    c = WSClient("127.0.0.1", svc.port)
    c.send_frame(0x2, b"\x00\x01\x02")
    closed, code = expect_close(c)
    check(closed and code == b"\x03\xeb", "binary data frame closes with 1003")
    c.close()

    c = WSClient("127.0.0.1", svc.port)
    c.send_frame(0x3, b"x")
    closed, code = expect_close(c)
    check(closed and code == b"\x03\xea", "reserved opcode closes with 1002")
    c.close()

    c = WSClient("127.0.0.1", svc.port)
    c.send_frame(0x1, b"\xff\xfe")
    closed, code = expect_close(c)
    check(closed and code == b"\x03\xef", "invalid UTF-8 text closes with 1007")
    c.close()

    # a well-formed multibyte JSON text frame still works
    c = WSClient("127.0.0.1", svc.port)
    c.send_json({"type": "spectate"})
    m = c.wait_state(timeout=3)
    check(m and m.get("type") == "state", "valid multibyte-capable JSON still accepted")
    c.close()


def test_envelope_strict(svc):
    """Untrusted commands must carry integer seq+epoch and a valid window."""
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    a, b = join_players(svc, ["Nia", "Oli"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    a.drain(); b.drain()
    cell = find_cell(a.gs, False)
    host.cmd("SELECT", target=cell)
    open_buzz_window(host, [a, b])
    a.drain()

    ep = a.state["epoch"]
    win = a.state["window"]
    last = max(a.seq, a.state.get("seq", 0))

    a.ws.send_json({"type": "cmd", "epoch": ep, "window": win, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "sequence" in e.get("error", ""), "omitted seq rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 10, "window": win, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "epoch" in e.get("error", ""), "omitted epoch rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 11, "epoch": str(ep), "window": win, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "epoch" in e.get("error", ""), "string epoch rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 12.5, "epoch": ep, "window": win, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "sequence" in e.get("error", ""), "fractional seq rejected")

    a.ws.send_json({"type": "cmd", "seq": 4294967296, "epoch": ep, "window": win, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "sequence" in e.get("error", ""), "out-of-range/wrap seq rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 13, "epoch": ep, "window": -1, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "window" in e.get("error", ""), "negative window rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 14, "epoch": ep, "window": 1.5, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "window" in e.get("error", ""), "fractional window rejected")

    a.ws.send_json({"type": "cmd", "seq": last + 15, "epoch": ep, "action": "BUZZ"})
    e = a.expect_error(); check(e is not None and "window" in e.get("error", ""), "omitted window rejected")

    hlast = max(host.seq, host.state.get("seq", 0))
    host.ws.send_json({"type": "cmd", "seq": hlast + 1, "epoch": host.state["epoch"], "action": "SELECT", "target": 0})
    e = host.expect_error(); check(e is not None and "window" in e.get("error", ""), "SELECT without window rejected")

    # a spectator cannot act at all
    spec = Player.spectate(svc)
    spec.ws.send_json({"type": "cmd", "seq": 1, "epoch": ep, "window": win, "action": "BUZZ"})
    e = spec.expect_error(); check(e is not None and "spectator" in e.get("error", ""), "spectator command rejected")
    spec.close()
    a.close(); b.close(); host.close()


def test_timeout_regression(svc):
    """A timer that expires first cannot be raced by a late action."""
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    host.cmd("CONFIG", config={
        "read_ms": 50, "buzz_ms": 300, "answer_ms": 3000,
        "dd_wager_ms": 500, "dd_answer_ms": 3000,
        "final_wager_ms": 500, "final_answer_ms": 3000, "tiebreak_answer_ms": 3000,
    })
    time.sleep(0.15); host.drain()
    a, b = join_players(svc, ["Pia", "Quinn"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    a.drain(); b.drain()
    board_win = host.state["window"]

    cell = find_cell(a.gs, False)
    host.cmd("SELECT", target=cell)
    open_buzz_window(host, [a, b])
    a.drain()
    old_win = a.state["window"]
    lat = max(a.seq, a.state.get("seq", 0))
    time.sleep(0.7)  # let the 300 ms buzz window expire and the service tick
    a.ws.send_json({"type": "cmd", "seq": lat + 100, "epoch": a.state["epoch"],
                    "window": old_win, "action": "BUZZ"})
    e = a.expect_error()
    check(e is not None and ("buzz" in e.get("error", "") or "clue" in e.get("error", "")),
          "late buzz after window expiry rejected")
    host.wait_phase(REVEAL, timeout=3)
    host.cmd("ADVANCE")
    host.wait_phase(BOARD, timeout=3)
    host.drain()

    # stale board selection: an old (pre-clue) window must not open a cell
    cell2 = find_cell(host.gs, False)
    hlast = max(host.seq, host.state.get("seq", 0))
    host.ws.send_json({"type": "cmd", "seq": hlast + 1, "epoch": host.state["epoch"],
                       "window": board_win, "action": "SELECT", "target": cell2})
    e = host.expect_error()
    check(e is not None and ("clue" in e.get("error", "") or "stale" in e.get("error", "")),
          "board selection with an old window rejected")

    # daily double: a wager after the wager timer expired is rejected
    dd = find_cell(host.gs, True)
    check(dd is not None, "daily double available for the timeout test")
    if dd is not None:
        host.cmd("SELECT", target=dd)
        host.wait_phase(DD_WAGER, timeout=3)
        sel_idx = host.gs["selector"]
        selector = a if host.players()[sel_idx]["id"] == a.player_id else b
        selector.wait(lambda gs: gs["phase"] == DD_WAGER, timeout=3)
        oldw = selector.state["window"]
        slast = max(selector.seq, selector.state.get("seq", 0))
        selector.wait(lambda gs: gs["phase"] == DD_CLUE, timeout=4)  # timer auto-advances
        selector.ws.send_json({"type": "cmd", "seq": slast + 100, "epoch": selector.state["epoch"],
                               "window": oldw, "action": "WAGER", "value": 100})
        e = selector.expect_error()
        check(e is not None and ("clue" in e.get("error", "") or "stale" in e.get("error", "")),
              "late DD wager after timer expiry rejected")
    a.close(); b.close(); host.close()


def test_public_display_clock(svc):
    """The private public display carries a monotonic now_ms for local countdown."""
    pin = svc.metadata["joinPin"]
    host = Player.host(svc, svc.metadata["hostPin"])
    configure_fast(host)
    join_players(svc, ["Rae", "Sam"], pin)
    host.cmd("START")
    host.wait_phase(BOARD)
    d = svc.public_display()
    check("window" in d, "public display carries the window id")
    check("now_ms" in d and isinstance(d["now_ms"], int) and d["now_ms"] > 0,
          "public display exposes a positive monotonic now_ms")
    check(isinstance(d.get("state"), dict) and "timer" in d["state"], "public display carries the redacted state")
    host.close()


def test_pin_throttle(svc):
    """Repeated wrong host PIN attempts are rate limited."""
    hit = 0
    for _ in range(80):
        st, _, _ = http_post("127.0.0.1", svc.port, "/api/host", {"pin": "000000"})
        if st == 429:
            hit = 1
            break
    check(hit == 1, "bad host PIN attempts are throttled with 429")


def main():
    if len(sys.argv) < 3:
        print("usage: test_network.py <jeopardy-host> <demo.jdb> [large.jdb]")
        return 2
    exe = os.path.abspath(sys.argv[1])
    demo = os.path.abspath(sys.argv[2])
    large = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else demo

    tmp = tempfile.mkdtemp(prefix="jpq-net-")

    def run(tag, fn, db=None):
        d = os.path.join(tmp, tag)
        os.makedirs(d, exist_ok=True)
        svc = Service(exe, d, db or demo)
        try:
            return fn(svc)
        finally:
            svc.stop()

    run("http", test_http_and_privacy_of_pins)
    run("join", test_join_capacity_and_validation)
    run("frames", test_framing_bounds)
    run("wsproto", test_ws_protocol_frames)
    run("cap", test_connection_cap)
    run("native", test_native_channel)
    run("buzz", test_buzz_race_and_auth)
    run("replay", test_replay_and_stale)
    run("envelope", test_envelope_strict)
    run("timeout", test_timeout_regression)
    run("clock", test_public_display_clock)
    run("throttle", test_pin_throttle)
    run("private", test_private_data)
    svc2 = run("reconnect", test_reconnect_and_restart)
    if svc2:
        svc2.stop()
    run("full", test_full_game, db=large)

    # explicit public URL must override LAN autodetection
    d = os.path.join(tmp, "url")
    os.makedirs(d, exist_ok=True)
    usvc = Service(exe, d, demo, env={"JPQ_PUBLIC_URL": "http://10.9.8.7:8135/"})
    try:
        check(usvc.metadata["metadataUrl"] == "http://10.9.8.7:8135/", "explicit JPQ_PUBLIC_URL is honored")
    finally:
        usvc.stop()

    print(f"network: {CHECKS} checks, {FAILS} failures")
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
