"""Minimal stdlib-only raw WebSocket client for the Jeopardy service tests.

Implements the RFC6455 client handshake and masked frame encoding, and parses
server frames (text/ping/pong/close) with an incremental buffer. No third-party
packages are used.
"""
from __future__ import annotations

import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WSError(Exception):
    pass


class WSClient:
    def __init__(self, host: str, port: int, path: str = "/ws", timeout: float = 6.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.buf = b""
        self.closed = False
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        head = self._read_until(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise WSError("upgrade failed: " + head.decode("latin1", "replace"))
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if accept.encode() not in head:
            raise WSError("bad Sec-WebSocket-Accept")

    # -- low level -----------------------------------------------------
    def _read_until(self, marker: bytes) -> bytes:
        while marker not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WSError("connection closed during handshake")
            self.buf += chunk
        idx = self.buf.index(marker) + len(marker)
        head, self.buf = self.buf[:idx], self.buf[idx:]
        return head

    def _recv_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WSError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_frame(self, opcode: int, payload: bytes = b"", fin: bool = True, mask: bool = True) -> None:
        b0 = (0x80 if fin else 0) | (opcode & 0x0F)
        header = bytes([b0])
        ln = len(payload)
        if ln < 126:
            header += bytes([(0x80 if mask else 0) | ln])
        elif ln <= 0xFFFF:
            header += bytes([(0x80 if mask else 0) | 126]) + struct.pack(">H", ln)
        else:
            header += bytes([(0x80 if mask else 0) | 127]) + struct.pack(">Q", ln)
        if mask:
            key = os.urandom(4)
            header += key
            payload = bytes(c ^ key[i % 4] for i, c in enumerate(payload))
        self.sock.sendall(header + payload)

    def send_text(self, text: str) -> None:
        self.send_frame(0x1, text.encode())

    def send_json(self, obj) -> None:
        self.send_text(json.dumps(obj))

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def recv_frame(self) -> tuple[int, bytes]:
        b = self._recv_exact(2)
        fin = b[0] >> 7
        opcode = b[0] & 0x0F
        masked = b[1] >> 7
        ln = b[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._recv_exact(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._recv_exact(8))[0]
        key = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(ln) if ln else b""
        if masked:
            payload = bytes(c ^ key[i % 4] for i, c in enumerate(payload))
        return opcode, payload

    def recv_json(self, timeout: float | None = None) -> dict:
        """Return the next JSON message, transparently answering pings."""
        deadline = time.time() + (timeout if timeout is not None else self.timeout)
        while True:
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                opcode, payload = self.recv_frame()
            except (socket.timeout, TimeoutError):
                raise WSError("timed out waiting for message")
            if opcode == 0x9:  # ping
                self.send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                self.closed = True
                raise WSError("server closed connection")
            if opcode == 0x1:
                return json.loads(payload.decode())

    def wait_state(self, pred=None, timeout: float = 6.0) -> dict:
        """Pump messages until a state message (optionally matching pred)."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            msg = self.recv_json(timeout=deadline - time.time())
            if msg.get("type") == "state":
                last = msg
                if pred is None or pred(msg):
                    return msg
            elif msg.get("type") == "error":
                last = msg
        if last is not None:
            return last
        raise WSError("no matching state")

    def try_recv_json(self, timeout: float = 0.5):
        try:
            return self.recv_json(timeout=timeout)
        except WSError:
            return None

    def close(self) -> None:
        if self.closed:
            return
        try:
            self.send_frame(0x8, b"\x03\xe8")
            self.sock.settimeout(0.5)
            try:
                while True:
                    op, _ = self.recv_frame()
                    if op == 0x8:
                        break
            except (WSError, socket.timeout, TimeoutError, OSError):
                pass
        except OSError:
            pass
        finally:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass


def http_request(host: str, port: int, method: str, path: str, body=None, timeout: float = 6.0):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    payload = None
    headers = {}
    if body is not None:
        payload = json.dumps(body)
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=payload, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    text = data.decode("utf-8", "replace")
    try:
        js = json.loads(text)
    except json.JSONDecodeError:
        js = None
    return resp.status, js, text


def http_get(host, port, path):
    return http_request(host, port, "GET", path)


def http_post(host, port, path, body):
    return http_request(host, port, "POST", path, body)
