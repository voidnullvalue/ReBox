#!/usr/bin/env python3
"""Standalone stand-in for the hr54-jf DOOM endpoints.

Milestone 1 runs this on the workstation so the ITV transport can be proven
without first rebuilding and redeploying the on-box backend.  The URL surface
is byte-for-byte the same as jellyfin/remote/hr54_jf.c, so the frontend that
gets proven against this is the frontend that ships.

    GET  /doom/               index.html
    GET  /doom/frontend.js
    GET  /doom/frame.bmp?s=N  live 320x200 24-bit BMP
    GET  /doom/frame.raw      HRDF frame, base64 or raw
    GET  /doom/ping
    POST /doom/input          keys=<bitmask>
    POST /doom/quit
"""

import base64
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

W, H = 320, 200
ROOT = Path(__file__).resolve().parent.parent / "itv"

_lock = threading.Lock()
_seq = 0
_held = 0
_fb = bytearray(W * H)
_pal = bytearray(1024)

# Same palette the native engine publishes, so what the TV shows now is what
# it will show when the real engine drives the same endpoint.
BASE = [
    (255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
    (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0),
    (255, 128, 0), (128, 255, 0), (0, 255, 128), (128, 0, 255),
    (255, 0, 128), (0, 128, 255), (128, 255, 255), (192, 192, 192),
]


def _init_palette():
    for i, (r, g, b) in enumerate(BASE):
        _pal[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    for i in range(16, 256):
        v = (i - 16) * 255 // 239
        _pal[i * 4:i * 4 + 4] = bytes((v, v, v, 255))


_init_palette()


def _rect(buf, x0, y0, w, h, idx):
    x0 = max(0, x0)
    y0 = max(0, y0)
    w = min(w, W - x0)
    h = min(h, H - y0)
    if w <= 0 or h <= 0:
        return
    row = bytes((idx,)) * w
    for y in range(y0, y0 + h):
        buf[y * W + x0:y * W + x0 + w] = row


def _render(n, held):
    """Colour bars, a grey ramp, sweepers, held-key cells, frame counter."""
    for x in range(W):
        v = 16 + x * 239 // (W - 1)
        for y in range(40):
            _fb[y * W + x] = v
    for i in range(8):
        _rect(_fb, i * 40, 40, 40, 40, i)
    _rect(_fb, 0, 80, W, H - 80, 7)
    _rect(_fb, (n * 3) % (W - 30), 84, 30, 30, 4)
    _rect(_fb, W - 30 - (n * 3) % (W - 30), 114, 30, 30, 6)
    for i in range(8):
        _rect(_fb, 4 + i * 14, 120, 12, 12, 3 if held & (1 << i) else 12)
    for i in range(16):
        _rect(_fb, 4 + i * 14, 138, 12, 12, 15 if n & (1 << i) else 8)
    _rect(_fb, (n * 5) % W, 156, 4, 40, 11)


def _advance():
    global _seq
    with _lock:
        _seq += 1
        _render(_seq, _held)
        return _seq, bytes(_fb), bytes(_pal)


def _hdr(seq, pal):
    return b"HRDF" + struct.pack("<IIII", 1, W, H, seq) + pal


def _bmp(fb, pal, seq):
    stride = W * 3
    pad = (4 - (stride & 3)) & 3
    size = 54 + (stride + pad) * H
    head = b"BM" + struct.pack("<IHHI", size, 0, 0, 54)
    head += struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, (stride + pad) * H,
                        2835, 2835, 0, 0, 0)
    body = bytearray()
    for y in range(H - 1, -1, -1):
        base = y * W
        for x in range(W):
            p = fb[base + x] * 4
            body += bytes((pal[p + 2], pal[p + 1], pal[p]))
        body += b"\x00" * pad
    return head + bytes(body)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        if "frame" in (args[0] if args else "") or "--quiet" in sys.argv:
            return
        sys.stderr.write("[doom-probe] %s\n" % (fmt % args))

    def _send(self, code, ctype, body, extra=""):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        if extra:
            self.send_header("X-Doom-Seq", extra)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        route = u.path
        since = q.get("since", ["-1"])[0]
        try:
            since = int(since)
        except ValueError:
            since = -1

        if route in ("/", "/doom", "/doom/", "/doom/index.html"):
            return self._file("index.html", "text/html; charset=utf-8")
        if route == "/doom/frontend.js":
            return self._file("frontend.js", "application/javascript")
        # The ITV renderer asks every app for this before it renders.  A 404
        # is harmless but noisy, and answering keeps the request log readable.
        if route.endswith("/NDS/iVGConfiguration"):
            return self._send(200, "text/plain", b"{}")
        if route == "/doom/ping":
            return self._send(200, "text/plain", b"ok")
        if route == "/doom/frame.bmp":
            seq, fb, pal = _advance()
            return self._send(200, "image/bmp", _bmp(fb, pal, seq), str(seq))
        if route == "/doom/frame.raw":
            seq, fb, pal = _advance()
            if since >= 0 and seq == since:
                self.send_response(304)
                self.send_header("Content-Length", "0")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            raw = _hdr(seq, pal) + fb
            if q.get("b64"):
                return self._send(200, "text/plain",
                                  base64.b64encode(raw), str(seq))
            return self._send(200, "application/octet-stream", raw, str(seq))
        self._send(404, "text/plain", b"not found")

    def do_POST(self):
        global _held
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("latin-1") if n else ""
        if u.path == "/doom/input":
            for k, v in parse_qs(body).items():
                if k == "keys":
                    try:
                        _held = int(v[0]) & 0xFF
                    except ValueError:
                        pass
            sys.stderr.write("[doom-probe] input held=%d\n" % _held)
            return self._send(200, "text/plain", b"ok")
        if u.path == "/doom/quit":
            sys.stderr.write("[doom-probe] quit requested\n")
            return self._send(200, "text/plain", b"ok")
        self._send(404, "text/plain", b"not found")

    def _file(self, name, ctype):
        data = (ROOT / name).read_bytes()
        self._send(200, ctype, data)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 8130
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.daemon_threads = True

    def spin():
        # Keep the sequence moving even when nobody is polling, so a late
        # viewer sees a live pattern rather than a frozen first frame.
        while True:
            time.sleep(1 / 15.0)
            _advance()

    threading.Thread(target=spin, daemon=True).start()
    sys.stderr.write("[doom-probe] listening on 0.0.0.0:%d -> %s\n" % (port, ROOT))
    srv.serve_forever()


if __name__ == "__main__":
    main()
