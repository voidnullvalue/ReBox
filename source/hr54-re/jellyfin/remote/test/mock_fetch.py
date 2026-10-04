#!/usr/bin/env python3
"""Test stand-in for the receiver's hr54-play-url decoder fetch.

Fetches the opaque /play/<token>.ts URL passed as argv[1] the way VODCapture
would: open, read until EOF, write bytes to MOck_FETCH_OUT.  Prints running
progress so pause gating (stalled reads) and stop (early EOF) are observable.
"""
import os
import socket
import sys
import time
from urllib.parse import urlsplit

out_path = os.environ.get("MOCK_FETCH_OUT", "/tmp/opencode/mock-fetch.out")
progress_path = os.environ.get("MOCK_FETCH_PROGRESS", "/tmp/opencode/mock-fetch.progress")

url = sys.argv[1]
u = urlsplit(url)
host = u.hostname
port = u.port or 80
req = "GET %s HTTP/1.0\r\nHost: %s:%d\r\nAccept: */*\r\nConnection: close\r\n\r\n" % (
    u.path + ("?" + u.query if u.query else ""), host, port)
s = socket.create_connection((host, port), timeout=120)
s.sendall(req.encode())

buf = b""
header_end = -1
while header_end < 0:
    data = s.recv(65536)
    if not data:
        break
    buf += data
    header_end = buf.find(b"\r\n\r\n")
if header_end < 0:
    print("no header", flush=True)
    sys.exit(1)
status_line = buf.split(b"\r\n")[0].decode()
print("status: %s" % status_line, flush=True)
if " 200 " not in status_line:
    print("non-200", flush=True)
    sys.exit(1)

body = buf[header_end + 4:]
total = len(body)
with open(out_path, "wb") as out:
    out.write(body)
    out.flush()
    while True:
        data = s.recv(65536)
        if not data:
            break
        out.write(data)
        out.flush()
        total += len(data)
        progress_tmp = progress_path + "." + str(os.getpid())
        with open(progress_tmp, "w") as pf:
            pf.write("%d %d\n" % (total, time.time()))
        os.replace(progress_tmp, progress_path)
print("fetched %d bytes" % total, flush=True)
s.close()
