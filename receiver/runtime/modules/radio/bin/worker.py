"""On-demand radio fetcher. Verified TLS, bounded HLS, no media disk writes."""
import os, sys, time, urllib.request, urllib.parse, subprocess, signal
ROOT = os.environ.get('JF_PERSIST_ROOT', '/var/hr54-persist/jellyfin')

def open_url(url):
    if urllib.parse.urlsplit(url).scheme not in ('http', 'https'):
        raise ValueError('Unsupported radio protocol')
    return urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'ReBox-Radio/1', 'Icy-MetaData': '0'}), timeout=20)

def feed(url, sink):
    seen = -1
    progressed = time.monotonic()
    for depth in range(6):
        with open_url(url) as response:
            effective = response.url
            first = response.read(7)
            if first.lower() == b'[playli':
                raw = first + response.read(1024 * 1024 + 1)
                if len(raw) > 1024 * 1024: raise ValueError('Radio playlist too large')
                entries = [s.split('=', 1)[1].strip() for s in raw.decode('utf-8-sig').splitlines()
                           if s.lower().startswith('file') and '=' in s]
                if not entries: raise ValueError('No station URL in PLS playlist')
                url = urllib.parse.urljoin(effective, entries[0])
                continue
            if first != b'#EXTM3U':
                sink.write(first)
                while True:
                    data = response.read1(32768)
                    if not data: return
                    sink.write(data)
            raw = first + response.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024: raise ValueError('Radio playlist too large')
        lines = [s.strip() for s in raw.decode('utf-8-sig').splitlines()]
        if any(s.startswith('#EXT-X-STREAM-INF:') for s in lines):
            # Choose the first rendition; audio-only stations generally have one.
            variant = next((s for s in lines if s and not s.startswith('#')), None)
            if not variant: raise ValueError('No radio rendition')
            url = urllib.parse.urljoin(effective, variant)
            continue
        break
    else: raise ValueError('Too many radio playlists')
    while True:
        sequence = 0
        for line in lines:
            if line.startswith('#EXT-X-KEY:') and 'METHOD=NONE' not in line:
                raise ValueError('Encrypted radio HLS unsupported')
            if line.startswith(('#EXT-X-MAP:', '#EXT-X-BYTERANGE:')):
                raise ValueError('Fragmented/byte-range radio HLS unsupported')
            if line.startswith('#EXT-X-MEDIA-SEQUENCE:'): sequence = int(line.split(':', 1)[1])
        segments = [s for s in lines if s and not s.startswith('#')]
        if len(segments) > 512: raise ValueError('Too many radio segments')
        start = max(0, len(segments) - 3) if seen < 0 and '#EXT-X-ENDLIST' not in lines else 0
        for i, segment in enumerate(segments):
            if i < start or sequence + i <= seen: continue
            with open_url(urllib.parse.urljoin(effective, segment)) as response:
                size = 0
                while True:
                    data = response.read1(32768)
                    if not data: break
                    size += len(data)
                    if size > 12 * 1024 * 1024: raise ValueError('Radio segment too large')
                    sink.write(data)
            seen = sequence + i
            progressed = time.monotonic()
        sink.flush()
        if '#EXT-X-ENDLIST' in lines: return
        if time.monotonic() - progressed > 40: raise ValueError('Radio stream stalled')
        time.sleep(1)
        with open_url(url) as response:
            effective = response.url
            raw = response.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024: raise ValueError('Radio playlist too large')
            lines = [s.strip() for s in raw.decode('utf-8-sig').splitlines()]

class ProbeComplete(Exception): pass

def probe(url):
    class Sink:
        def __init__(self): self.head = bytearray()
        def write(self, data):
            self.head.extend(data[:4096 - len(self.head)])
            if len(self.head) >= 376: raise ProbeComplete()
        def flush(self): pass
    sink = Sink()
    try: feed(url, sink)
    except ProbeComplete: pass
    head = sink.head
    if (head.startswith((b'ID3', b'OggS', b'fLaC')) or
        len(head) > 1 and head[0] == 255 and head[1] & 224 == 224 or
        len(head) > 188 and head[0] == head[188] == 71): return
    raise ValueError('Station did not return recognizable radio audio')

if __name__ == '__main__':
    def cancelled(*args): raise SystemExit(0)
    signal.signal(signal.SIGTERM, cancelled)
    if sys.argv[1] == '--probe':
        probe(sys.argv[2])
        sys.exit(0)
    mux = subprocess.Popen([os.environ.get('REBOX_RADIO_MUX', ROOT + '/radio/bin/hr54-radio-remux')], stdin=subprocess.PIPE, bufsize=0)
    try:
        feed(sys.argv[1], mux.stdin)
        mux.stdin.close()
        if mux.wait() != 0: raise RuntimeError('Radio audio unsupported')
    finally:
        if mux.poll() is None: mux.kill()
        mux.wait()
