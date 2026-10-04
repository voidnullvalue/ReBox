#!/usr/bin/env python3
"""Mock Jellyfin server for testing the receiver-native hr54-jf service.

Implements only the endpoints hr54-jf uses, mirroring the response shapes the
real server produced during the reference-implementation work.  Quick Connect
approval is simulated by creating APPROVE_FILE.
"""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

TOKEN = "goodtoken-goodtoken-goodtoken"
USER_ID = "u1234abcd"
APPROVE_FILE = os.environ.get("MOCK_APPROVE_FILE", "/tmp/opencode/mock-approved")
REQUEST_LOG = os.environ.get("MOCK_REQUEST_LOG", "/tmp/opencode/mock-requests.log")
TS_BYTES = int(os.environ.get("MOCK_TS_BYTES", "6") ) * 1024 * 1024


def note(path, extra=""):
    with open(REQUEST_LOG, "a") as fh:
        fh.write("%s %s\n" % (path, extra))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        pass

    def token_ok(self):
        return self.headers.get("X-Emby-Token") == TOKEN

    def reply_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        route = urlsplit(self.path).path
        if route == "/QuickConnect/Initiate":
            note("QuickConnect/Initiate")
            return self.reply_json({"Secret": "mocksecret123", "Code": "MOCKQC"})
        if route == "/Users/AuthenticateWithQuickConnect":
            payload = json.loads(raw or b"{}")
            if payload.get("Secret") != "mocksecret123":
                return self.reply_json({}, 401)
            note("AuthenticateWithQuickConnect")
            return self.reply_json({
                "AccessToken": TOKEN,
                "User": {"Id": USER_ID, "Name": "void"},
            })
        if not self.token_ok():
            return self.reply_json({"error": "unauthorized"}, 401)
        if route.startswith("/Items/") and route.endswith("/PlaybackInfo"):
            item = route.split("/")[2]
            url = "/videos/%s/stream.ts?DeviceId=mock&PlaySessionId=sess1&StartTimeTicks=0&CopyTimestamps=true" % item
            payload = {}
            try:
                payload = json.loads(raw or b"{}")
            except ValueError:
                pass
            ceiling = payload.get("MaxStreamingBitrate")
            profile = payload.get("DeviceProfile", {})
            assert ceiling in (8000000, 12000000, 16000000)
            assert profile.get("MaxStreamingBitrate") == ceiling
            assert profile.get("DirectPlayProfiles") == []
            assert not payload.get("EnableDirectPlay") and not payload.get("EnableDirectStream")
            assert payload.get("AllowAudioStreamCopy") is False
            assert payload.get("AllowVideoStreamCopy") == (payload.get("StartTimeTicks", 0) == 0)
            note("Quality/%s" % item, "top=%s profile=%s videoCopy=%s audioCopy=%s" % (ceiling, profile.get("MaxStreamingBitrate"), payload.get("AllowVideoStreamCopy"), payload.get("AllowAudioStreamCopy")))
            note("PlaybackInfo/%s" % item, "UserId=%s StartTimeTicks=%s" % (payload.get("UserId", ""), payload.get("StartTimeTicks", "missing")))
            return self.reply_json({"MediaSources": [{
                "TranscodingUrl": url,
                "TranscodingContainer": "ts",
            }]})
        return self.reply_json({"error": "not found"}, 404)

    def do_GET(self):
        parts = urlsplit(self.path)
        route = parts.path
        query = parse_qs(parts.query)
        if route == "/QuickConnect/Enabled":
            note("QuickConnect/Enabled")
            return self.reply_json(True)
        if route == "/QuickConnect/Connect":
            note("QuickConnect/Connect", "Secret=%s" % query.get("Secret", [""])[0])
            approved = os.path.exists(APPROVE_FILE)
            return self.reply_json({"Authenticated": approved})
        if route == "/Users/Me":
            if not self.token_ok():
                return self.reply_json({"error": "unauthorized"}, 401)
            note("Users/Me")
            return self.reply_json({"Id": USER_ID, "Name": "void"})
        if route == "/Users/%s/Views" % USER_ID:
            if not self.token_ok():
                return self.reply_json({"error": "unauthorized"}, 401)
            note("Users/Views")
            return self.reply_json({"Items": [
                {"Id": "lib1", "Name": "Movies", "CollectionType": "movies"},
                {"Id": "lib2", "Name": 'Shows & "Things"', "CollectionType": "tvshows"},
            ]})
        if route == "/Users/%s/Items" % USER_ID:
            if not self.token_ok():
                return self.reply_json({"error": "unauthorized"}, 401)
            term = query.get("SearchTerm", [""])[0]
            limit = int(query.get("Limit", ["60"])[0])
            offset = int(query.get("StartIndex", ["0"])[0])
            note("Users/Items", "SearchTerm=%r Limit=%s StartIndex=%s video=%s" % (
                term, limit, offset, query.get("IncludeItemTypes", [""])[0]))
            items = [
                {"Id": "vid1", "Name": 'Title "One" \\ Händel & Co', "Type": "Movie",
                 "ProductionYear": 2019, "RunTimeTicks": 72000000000,
                 "MediaType": "Video", "LocationType": "Original",
                 "Overview": "x" * 900},
                {"Id": "vid2", "Name": "Second", "Type": "Episode",
                 "MediaType": "Video", "LocationType": "Original"},
                {"Id": "virt1", "Name": "Virtual Thing", "Type": "Episode",
                 "MediaType": "Video", "LocationType": "Virtual"},
            ]
            if term:
                items = [i for i in items if term.lower() in i["Name"].lower()]
            return self.reply_json({"TotalRecordCount": len(items),
                                    "Items": items[offset:offset + limit]})
        if route.startswith("/Items/") and route.endswith("/Images/Primary"):
            item = route.split("/")[2]
            if not self.token_ok():
                return self.reply_json({"error": "unauthorized"}, 401)
            note("Art/%s" % item)
            body = b"\xff\xd8\xff\xe0MOCKJPEG" + item.encode() + b"\xff\xd9"
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if route.startswith("/Items/"):
            item = route.split("/")[2]
            if not self.token_ok():
                return self.reply_json({"error": "unauthorized"}, 401)
            note("Items/%s" % item)
            return self.reply_json({
                "Id": item, "Name": "Title One", "MediaType": "Video",
                "LocationType": "Original",
                "MediaSources": [{"MediaStreams": [{"Type": "Video"}]}],
            })
        if route.startswith("/videos/") and route.endswith("/stream.ts"):
            item = route.split("/")[2]
            ticks = query.get("StartTimeTicks", ["0"])[0]
            if len(query.get("StartTimeTicks", [])) != 1:
                return self.reply_json({"error":"ambiguous start offset"}, 400)
            note("TranscodeGet/%s" % item, "StartTimeTicks=%s" % ticks)
            if int(ticks) > 0 and query.get("CopyTimestamps") != ["true"]:
                return self.reply_json({"error":"seek input timestamps were not preserved"}, 400)
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.end_headers()
            chunk = b"\x47" + b"T" * 187  # TS packet sized filler
            written = 0
            while written < TS_BYTES:
                self.wfile.write(chunk)
                written += len(chunk)
                self.wfile.flush()
                time.sleep(0.002)  # throttle so gating is observable
            return
        return self.reply_json({"error": "not found"}, 404)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18096
    for path in (APPROVE_FILE, REQUEST_LOG):
        try:
            os.remove(path)
        except OSError:
            pass
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    print("mock jellyfin on 127.0.0.1:%d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
