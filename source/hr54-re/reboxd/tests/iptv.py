"""Real IPTV RPC, curl helper and bounded TS/HLS relay with a local upstream."""
import collections
import http.server
import json
import shutil
import socket
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import runtime

ROOT = runtime.ROOT
# Minimal complete PMT declaring H.264. No receiver decoder is emulated here.
def transport_stream():
    packets = bytearray(b'\xff' * (188 * 8))
    for i in range(8):
        at = i * 188
        packets[at:at+4] = bytes([0x47, 0x01, 0x02, 0x10 | i])
    packets[1:5] = bytes([0x41, 0, 0x10, 0])
    pmt = bytes([2, 0xb0, 18, 0, 1, 0xc1, 0, 0, 0xe1, 2, 0xf0, 0, 0x1b, 0xe1, 2, 0xf0, 0, 0, 0, 0, 0])
    packets[5:5+len(pmt)] = pmt
    return bytes(packets)

class Iptv(unittest.TestCase):
    start = runtime.Runtime.start
    stop = runtime.Runtime.stop
    get = runtime.Runtime.get
    def setUp(self):
        runtime.Runtime.setUp(self)
        self.requests = []; self.refresh = collections.Counter(); self.slow = threading.Event()
        owner = self
        class Upstream(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_GET(self):
                owner.requests.append((self.path, self.headers.get('User-Agent'), self.headers.get('Referer')))
                if self.path == '/slow':
                    owner.slow.set(); time.sleep(2); self.send_error(503); return
                if self.path == '/start':
                    self.send_response(302); self.send_header('Location', '/a/master.m3u8?token=Q'); self.end_headers(); return
                if 'master.m3u8' in self.path:
                    data = b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=26000000,CODECS="avc1.640028"\nbig.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=4000000,CODECS="avc1.640028,mp4a.40.2"\n../low/live.m3u8?token=Q\n'
                elif 'live.m3u8' in self.path:
                    owner.refresh['live'] += 1
                    start = 10 + max(0, owner.refresh['live']-2)
                    data = ('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:'+str(start)+'\n'+''.join(f'#EXTINF:1,\nseg{i}.ts?token=Q\n' for i in range(start, start+2))).encode()
                elif self.path == '/vod.m3u8': data = b'#EXTM3U\n#EXTINF:1,\nseg20.ts\n#EXT-X-ENDLIST\n'
                elif self.path == '/fmp4': data = b'#EXTM3U\n#EXT-X-MAP:URI="init.mp4"\n#EXTINF:1,\na.m4s\n'
                elif self.path == '/encrypted': data = b'#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI="key"\n#EXTINF:1,\na.ts\n'
                elif '.ts' in self.path: data = transport_stream()
                elif self.path == '/logo': data = b'\x89PNG\r\n\x1a\nfixture'
                else: self.send_error(404); return
                self.send_response(200); self.send_header('Content-Length', str(len(data))); self.send_header('Content-Type', 'image/png' if self.path == '/logo' else 'application/octet-stream'); self.end_headers()
                try: self.wfile.write(data)
                except BrokenPipeError: pass
        self.upstream = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
        threading.Thread(target=self.upstream.serve_forever, daemon=True).start()
        self.addCleanup(self.upstream.server_close); self.addCleanup(self.upstream.shutdown)
        port = self.upstream.server_port
        module = self.root/'modules/iptv'; (module/'bin').mkdir(parents=True)
        shutil.copy(ROOT/'../modules/iptv/module.json', module/'module.json')
        shutil.copy(ROOT/'build/iptv-module', module/'bin/module')
        shutil.copy(ROOT/'build/iptv-fetch', module/'bin/fetch')
        self.data = self.root/'module-data/iptv'; (self.data/'iptv').mkdir(parents=True)
        self.playlist = '#EXTM3U\n'+''.join(f'#EXTINF:-1 tvg-id="{name}" tvg-logo="http://127.0.0.1:{port}/logo" group-title="{group}" http-user-agent="attribute, UA",{name}\n#EXTVLCOPT:http-user-agent=Exact, UA\n#EXTVLCOPT:http-referrer=https://ref.example/a?q=1,2\n{url if url.startswith("srt:") else "http://127.0.0.1:"+str(port)+"/"+url}\n' for name, url, group in [('Live','start','News, local'),('VOD','vod.m3u8',''),('fMP4','fmp4','Test'),('Encrypted','encrypted','Test'),('DASH','x.mpd','Test'),('SRT','srt://example.invalid:9999','Test'),('Slow','slow','Test')])
        (self.data/'iptv/eng.m3u').write_text(self.playlist)
        state = self.root/'module-state'; state.mkdir(); (state/'iptv.json').write_text('{"schema":1,"enabled":true}')
        self.start(); self.prefix = '/api/modules/iptv'; self.master = (state/'management.secret').read_text()
        self.channels = {c['name']: c['id'] for c in self.get('/api/iptv/channels?limit=60')['channels']}

    def wait(self, predicate):
        end=time.monotonic()+5
        while time.monotonic()<end:
            if predicate():return
            time.sleep(.05)
        self.fail('operation did not complete')

    def play(self, name, legacy=False):
        state = self.get('/api/iptv/play' if legacy else self.prefix+'/play', {'channelId' if legacy else 'itemId': self.channels[name]})
        self.assertTrue(state['playing']); self.assertEqual(state['source'], 'iptv'); self.assertTrue(state['live'])
        return state

    def stream(self):
        token = self.get(self.prefix+'/status')['playback']['session']
        response = urllib.request.urlopen(self.url+'/module-stream/iptv/'+token+'.ts', timeout=5)
        self.addCleanup(response.close)
        return response

    def test_root_metadata_search_pagination_reload_state(self):
        root = self.get(self.prefix+'/browse'); self.assertEqual(root['items'][0]['title'], 'All channels')
        self.assertTrue(all(i['kind']=='folder' for i in root['items']))
        group = root['items'][1]['id']; self.assertEqual(self.get(self.prefix+'/browse?parent='+group)['items'][0]['title'], 'Live')
        page = self.get(self.prefix+'/browse?parent=all&limit=2&offset=2'); self.assertEqual(len(page['items']), 2); self.assertTrue(page['hasMore']); self.assertEqual(page['total'], 7)
        row = self.get(self.prefix+'/search?q=local')['items'][0]; self.assertEqual(row['id'], self.channels['Live']); self.assertEqual(row['artwork'], row['id'])
        groups = self.get('/api/iptv/groups'); self.assertEqual(groups['groupCount'], 3); self.assertEqual(groups['groups'][1]['label'], 'Other channels')
        self.get('/api/iptv/state', {'group':'News, local', 'page':3, 'index':4})
        before = (self.data/'state/iptv-state.json').read_bytes()
        self.assertEqual(self.get('/api/iptv/state')['page'], 3)
        (self.data/'iptv/eng.m3u').write_text(self.playlist.replace(',Live\n', ',Renamed\n'))
        self.assertEqual(self.get(self.prefix+'/search?q=Renamed')['items'][0]['id'], self.channels['Live'])
        self.stop(); self.start(); self.assertEqual((self.data/'state/iptv-state.json').read_bytes(), before)
        self.assertEqual((self.data/'iptv/eng.m3u').read_text(), self.playlist.replace(',Live\n', ',Renamed\n'))

    def test_hls_bytes_headers_refresh_stop_and_legacy_play(self):
        state = self.play('Live', True); self.assertEqual(state['channelId'], self.channels['Live']); self.assertEqual(state['channelName'], 'Live')
        stream = self.stream(); self.assertEqual(stream.read(188), transport_stream()[:188])
        time.sleep(1.3)
        self.assertTrue(self.get('/api/capabilities')['iptv']); self.assertTrue(self.get('/api/system/status')['ready'])
        for operation in ['pause', 'resume', 'seek']:
            with self.assertRaises(urllib.error.HTTPError) as error: self.get('/api/playback/'+operation, {})
            self.assertEqual(error.exception.code, 409); error.exception.close()
        self.assertTrue(self.get('/api/iptv/status')['active'])
        segments = [p for p, _, _ in self.requests if '.ts?' in p]; self.assertGreaterEqual(len(segments), 3); self.assertEqual(len(segments), len(set(segments)))
        self.assertTrue(all(ua=='Exact, UA' and ref=='https://ref.example/a?q=1,2' for _,ua,ref in self.requests))
        self.assertIn('/a/master.m3u8?token=Q', [p for p,_,_ in self.requests]); self.assertFalse(any('big.m3u8' in p for p,_,_ in self.requests))
        self.assertTrue(self.get('/api/iptv/stop', {})['stopped']); self.assertFalse(self.get('/api/state')['playing'])
        time.sleep(.3); before=len(self.requests); time.sleep(.7); self.assertEqual(len(self.requests), before)
        self.assertTrue(self.get('/api/iptv/stop', {})['stopped'])

    def test_vod_eof_invalid_protocol_and_preparation_cancellation(self):
        self.play('VOD'); stream=self.stream(); self.assertEqual(stream.read(), transport_stream())
        self.wait(lambda: not self.get('/api/state')['playing'])
        for name in ['fMP4', 'Encrypted', 'DASH', 'SRT']:
            with self.assertRaises(urllib.error.HTTPError) as error: self.play(name)
            self.assertEqual(error.exception.code, 502); error.exception.close()
            self.assertFalse(self.get('/api/state')['playing'])
        result=[]
        def starting():
            try:self.play('Slow')
            except urllib.error.HTTPError as error:result.append(error.code); error.close()
        worker=threading.Thread(target=starting); worker.start(); self.assertTrue(self.slow.wait(2))
        before=time.monotonic(); self.get('/api/iptv/stop', {}); worker.join(3)
        self.assertLess(time.monotonic()-before, 1.5); self.assertFalse(worker.is_alive()); self.assertEqual(result, [502]); self.assertFalse(self.get('/api/state')['playing'])

    def test_stale_stop_management_and_data_retention(self):
        self.play('Live')
        with socket.socket(socket.AF_UNIX) as rpc:
            rpc.connect(str(self.socketdir/'iptv.sock'))
            body=b'{"session":"stale-session"}'
            rpc.sendall(b'POST /playback/stop HTTP/1.0\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
            self.assertEqual(rpc.recv(4096).split(b' ')[1], b'200')
        self.assertTrue(self.get(self.prefix+'/status')['active'])
        self.get('/api/playback/stop', {})
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/actions/playlist.reload', {})
        self.assertEqual(error.exception.code, 401); error.exception.close()
        def authorized(path, method='POST'):
            request=urllib.request.Request(self.url+path, data=b'{}', method=method, headers={'Content-Type':'application/json', 'Authorization':'Bearer '+self.master})
            with urllib.request.urlopen(request) as response:return json.load(response)
        self.assertIn('Loaded 7', authorized(self.prefix+'/actions/playlist.reload')['message'])
        authorized(self.prefix+'/disable'); self.assertFalse(self.get('/api/capabilities')['iptv'])
        self.assertTrue(self.get('/api/system/status')['ready'])
        authorized(self.prefix+'/enable'); self.assertTrue(self.get('/api/capabilities')['iptv'])
        authorized(self.prefix, 'DELETE'); self.assertFalse(self.get('/api/capabilities')['iptv'])
        self.assertEqual((self.data/'iptv/eng.m3u').read_text(), self.playlist)

    def test_missing_playlist_keeps_core_and_module_ready(self):
        self.stop(); (self.data/'iptv/eng.m3u').unlink(); self.start()
        self.assertTrue(self.get('/api/system/status')['ready']); self.assertTrue(self.get('/api/modules')['modules'][0]['healthy'])
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/browse')
        self.assertEqual(error.exception.code, 502); error.exception.close()

if __name__ == '__main__': unittest.main()
