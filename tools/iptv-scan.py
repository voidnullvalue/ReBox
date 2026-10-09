#!/usr/bin/env python3
"""Bounded upstream/format diagnosis. Never print or persist media URLs.

The scanner cannot establish HDMI video or audible output; 'media_verified'
means FFprobe decoded container metadata and found receiver candidate codecs.
"""
import argparse
from collections import Counter
import json
import re
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

MAX_RESPONSE = 12 * 1024 * 1024
MAX_PLAYLIST = 512 * 1024

def safe_name(name):
    name = re.sub(r'https?://\S+', '[redacted URL]', name, flags=re.IGNORECASE)
    name = re.sub(r'(?i)\b(token|key|sig|auth|password|pwd|secret)\s*=\s*[^\s,]+', r'\1=[redacted]', name)
    return name[:120]

def entries(path):
    meta = None
    for line in open(path, encoding='utf-8-sig', errors='replace'):
        line = line.strip()
        if line.startswith('#EXTINF:'):
            meta = {'name': safe_name(line.rsplit(',', 1)[-1]), 'ua': '', 'ref': ''}
            for key, field in [('http-user-agent', 'ua'), ('http-referrer', 'ref')]:
                match = re.search(r'\b' + key + r'="([^"]*)"', line)
                if match: meta[field] = match[1]
        elif meta and line.startswith('#EXTVLCOPT:http-user-agent='):
            meta['ua'] = line.partition('=')[2]
        elif meta and line.startswith('#EXTVLCOPT:http-referrer='):
            meta['ref'] = line.partition('=')[2]
        elif meta and line and not line.startswith('#'):
            meta['url'] = line
            yield meta
            meta = None

def fetch(url, entry, limit=MAX_RESPONSE, partial=False):
    headers = {'User-Agent': entry['ua'] or 'HR54-IPTV/1'}
    if entry['ref']: headers['Referer'] = entry['ref']
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=6) as response:
        data = response.read(limit + 1)
        if len(data) > limit and not partial: raise ValueError('oversized')
        return response.url, data[:limit]

def probe_media(data, audio_required=False):
    try:
        run = subprocess.run(['ffprobe', '-v', 'error', '-probesize', '2000000',
                              '-analyzeduration', '3000000', '-show_entries',
                              'stream=codec_type,codec_name,width,height,profile,level',
                              '-of', 'json', '-i', 'pipe:0'], input=data,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=8)
        tracks = json.loads(run.stdout or b'{}').get('streams', [])
    except (subprocess.TimeoutExpired, ValueError):
        return 'unknown_media', 'FFprobe could not inspect sample'
    videos = [t for t in tracks if t.get('codec_type') == 'video']
    audios = [t for t in tracks if t.get('codec_type') == 'audio']
    if not videos: return 'unknown_media', 'No video track found in bounded sample'
    video = videos[0]
    if video.get('codec_name') != 'h264':
        return 'unsupported_video_codec', video.get('codec_name', 'unknown')
    if video.get('width', 0) > 1920 or video.get('height', 0) > 1080:
        return 'unsupported_video_dimensions', f"{video.get('width')}x{video.get('height')}"
    for audio in audios:
        if audio.get('codec_name') not in ('aac', 'ac3'):
            return 'unsupported_audio_codec', audio.get('codec_name', 'unknown')
    if audio_required and not audios:
        return 'separate_audio_unhandled', 'Video rendition has no audio; master advertises a separate audio group'
    return 'media_verified', 'H.264' + ('/' + ','.join(a['codec_name'] for a in audios) if audios else ' video only')

def classify_http(error):
    code = error.code
    if code == 403: return 'http_403', 'Provider rejected request; geo or headers may be involved'
    if code == 404: return 'http_404', 'Media URL unavailable'
    if code == 429: return 'http_429', 'Provider rate limited request'
    if code >= 500: return 'http_5xx', 'Provider error'
    return 'http_other', f'HTTP {code}'

def inspect(entry):
    url = entry['url']
    audio_required = False
    if url.startswith('srt:'): return 'unsupported_protocol', 'SRT'
    if '.mpd' in urllib.parse.urlsplit(url).path.lower(): return 'dash', 'DASH manifest'
    for depth in range(4):
        base, data = fetch(url, entry, MAX_PLAYLIST if depth else 2 * 1024 * 1024, partial=depth == 0)
        if not data.startswith(b'#EXTM3U'):
            if data.startswith(b'\x47') and len(data) > 376 and data[188] == 0x47:
                return probe_media(data, audio_required)
            if b'<MPD' in data[:500]: return 'dash', 'DASH manifest'
            if b'<html' in data[:500].lower(): return 'html_instead_of_media', 'HTML response'
            return 'unsupported_container', 'Unknown direct media format'
        text = data.decode('utf-8-sig', 'replace')
        if '#EXT-X-KEY:' in text and 'METHOD=NONE' not in text:
            return 'encrypted', 'HLS encryption/DRM advertised'
        if '#EXT-X-STREAM-INF:' in text:
            lines = [s.strip() for s in text.splitlines()]
            variants = []
            for i, line in enumerate(lines[:-1]):
                if not line.startswith('#EXT-X-STREAM-INF:'): continue
                codecs = re.search(r'CODECS="([^"]*)"', line)
                bw = re.search(r'BANDWIDTH=(\d+)', line)
                if codecs and any(c in codecs[1].lower() for c in ('hvc1', 'hev1', 'vp09', 'av01', 'mp4a.40.5', 'ec-3')): continue
                if not lines[i + 1] or lines[i + 1].startswith('#'): continue
                variants.append((int(bw[1]) if bw else 0, urllib.parse.urljoin(base, lines[i + 1]), bool(re.search(r'\bAUDIO=', line))))
            if not variants: return 'no_compatible_variant', 'Manifest advertises no H.264 candidate'
            variants.sort(key=lambda v: abs(v[0] - 4000000))
            url = variants[0][1]
            audio_required = variants[0][2]
            continue
        if '#EXT-X-BYTERANGE:' in text: return 'hls_byte_range', 'Byte-range HLS'
        map_match = re.search(r'#EXT-X-MAP:.*?URI="([^"]+)"', text)
        if map_match:
            _, init = fetch(urllib.parse.urljoin(base, map_match[1]), entry)
            if b'moov' not in init[:MAX_RESPONSE]: return 'malformed_hls', 'Missing MP4 initialization'
        segments = [s.strip() for s in text.splitlines() if s.strip() and not s.startswith('#')]
        if not segments: return 'malformed_hls', 'No segments'
        _, segment = fetch(urllib.parse.urljoin(base, segments[-1]), entry)
        if map_match:
            if b'moof' not in segment[:256]: return 'unexpected_segment', 'Expected fragmented MP4'
            return probe_media(init + segment, audio_required)
        if not (segment.startswith(b'\x47') and len(segment) > 376 and segment[188] == 0x47):
            return 'unexpected_segment', 'Expected MPEG-TS'
        return probe_media(segment, audio_required)
    return 'malformed_hls', 'Nested HLS manifests exceed limit'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('playlist')
    parser.add_argument('--limit', type=int, default=48, help='Maximum channels (default 48)')
    parser.add_argument('--stride', type=int, default=1, help='Sample every Nth channel')
    parser.add_argument('--delay', type=float, default=0.6, help='Seconds between channels')
    parser.add_argument('--output', help='Write redacted JSON summary')
    args = parser.parse_args()
    if not 1 <= args.limit <= 200 or not 1 <= args.stride <= 1000 or args.delay < 0.2:
        parser.error('limit 1..200, stride 1..1000, delay >= 0.2 required')
    results = []
    number = -1
    for number, entry in enumerate(entries(args.playlist)):
        if number % args.stride or len(results) >= args.limit: continue
        try: category, detail = inspect(entry)
        except urllib.error.HTTPError as error: category, detail = classify_http(error)
        except urllib.error.URLError as error:
            reason = str(error.reason).lower()
            category = 'tls' if 'ssl' in reason or 'certificate' in reason else 'dns' if 'name' in reason else 'connection'
            detail = category + ' failure'
        except (TimeoutError, ConnectionError): category, detail = 'connection', 'Connection timed out or closed'
        except ValueError: category, detail = 'oversized', 'Upstream response exceeded bound'
        results.append({'index': number, 'name': entry['name'], 'category': category, 'detail': detail})
        time.sleep(args.delay)
    summary = {'playlist_entries': number + 1, 'tested': len(results),
               'media_verified': sum(r['category'] == 'media_verified' for r in results),
               'failures': dict(Counter(r['category'] for r in results if r['category'] != 'media_verified')),
               'results': results,
               'qualification': 'Media verified by host FFprobe only; receiver HDMI/audio unverified.'}
    encoded = json.dumps(summary, indent=2)
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as out: out.write(encoded + '\n')
    print(encoded)

if __name__ == '__main__': main()
