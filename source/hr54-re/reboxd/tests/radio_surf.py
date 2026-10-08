import sys,unittest,json,shutil,os,subprocess,http.server,threading,time,urllib.request,urllib.error
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import runtime,iptv
ROOT=runtime.ROOT
class Surf(iptv.Iptv):
    def test_adjacent_preserves_stream_on_failure_and_wraps(self):
        self.play('Live');stream=self.stream();self.assertEqual(stream.read(188),iptv.transport_stream()[:188])
        # Previous channel is the intentionally unreachable Slow entry.
        with self.assertRaises(urllib.error.HTTPError):self.get('/api/playback/channelDown',{})
        self.assertTrue(self.get('/api/state')['playing']);self.assertEqual(self.get('/api/state')['title'],'Live')
        (self.data/'iptv/eng.m3u').write_text('\n'.join(self.playlist.splitlines()[:9])+'\n')
        self.assertEqual(self.get('/api/playback/channelDown',{})['title'],'VOD')
        self.assertEqual(self.get('/api/playback/channelUp',{})['title'],'Live')
class Radio(runtime.Runtime):
    def test_radio_stream_and_fullscreen_transport(self):
        d=self.root/'modules/radio';(d/'bin').mkdir(parents=True);(d/'default-data/radio').mkdir(parents=True)
        shutil.copy(ROOT/'../modules/radio/assets/radio.png',d/'icon.png');shutil.copy(ROOT/'../modules/radio/module.json',d/'module.json');shutil.copy(ROOT/'build/radio-module',d/'bin/module');shutil.copy(ROOT/'../modules/radio/worker.py',d/'bin/worker.py')
        shutil.copy(ROOT/'build/radio-audio',d/'bin/audio-remux');shutil.copy(ROOT/'../modules/radio/assets/black.ts',d/'bin/black.ts')
        legacy=self.base/'legacy';(legacy/'youtube/bin').mkdir(parents=True);py=legacy/'youtube/bin/python';py.write_text('#!/bin/sh\nunset PYTHONHOME\nexec /usr/bin/python3 "$@"\n');py.chmod(0o700);shutil.copy(py,d/'bin/python')
        
        subprocess.run(['ffmpeg','-nostdin','-v','error','-f','lavfi','-i','sine=sample_rate=48000','-t','2','-c:a','libmp3lame',str(self.base/'tone.mp3')],check=True);data=(self.base/'tone.mp3').read_bytes()
        class Up(http.server.BaseHTTPRequestHandler):
            def log_message(self,*a):pass
            def do_GET(self):
                if self.path == '/unavailable': self.send_error(503);return
                if self.path == '/slow': time.sleep(2)
                self.send_response(200);self.end_headers()
                try:
                    for i in range(400):self.wfile.write(data);self.wfile.flush();time.sleep(.02)
                except (BrokenPipeError,ConnectionResetError):pass
        up=http.server.ThreadingHTTPServer(('127.0.0.1',0),Up);threading.Thread(target=up.serve_forever,daemon=True).start();self.addCleanup(up.server_close);self.addCleanup(up.shutdown)
        (d/'default-data/radio/stations.m3u').write_text('#EXTM3U\n'+''.join(f'#EXTINF:-1 group-title="Jazz",Station {i}\nhttp://127.0.0.1:{up.server_port}/{i}\n' for i in range(2)))
        (self.root/'module-state').mkdir();(self.root/'module-state/radio.json').write_text('{"schema":1,"enabled":true}');self.start()
        prefix='/api/modules/radio';self.assertEqual(self.get(prefix+'/browse')['items'][0]['title'],'All stations');rows=self.get(prefix+'/browse?parent=all')['items'];self.assertEqual(len(rows),2)
        self.get(prefix+'/play',{'itemId':rows[0]['id']});token=self.get(prefix+'/status')['playback']['session']
        stream=urllib.request.urlopen(self.url+'/module-stream/radio/'+token+'.ts',timeout=15);self.addCleanup(stream.close);self.assertEqual(stream.read(188)[:1],b'G')
        self.assertEqual(self.get('/api/playback/channelDown',{})['title'],'Station 1');self.assertEqual(self.get('/api/playback/channelUp',{})['title'],'Station 0');
        catalog=self.root/'module-data/radio/radio/stations.m3u';catalog.write_text(catalog.read_text()+f'#EXTINF:-1,Unavailable\nhttp://127.0.0.1:{up.server_port}/unavailable\n')
        with self.assertRaises(urllib.error.HTTPError): self.get('/api/playback/channelDown',{})
        self.assertTrue(self.get('/api/state')['playing']);self.assertEqual(self.get('/api/state')['title'],'Station 0')
        self.assertEqual(self.get('/api/playback/channelDown',{})['title'],'Station 1')
        with self.assertRaises(urllib.error.HTTPError): self.get('/api/playback/channelUp',{})
        self.assertEqual(self.get('/api/playback/channelUp',{})['title'],'Station 0')
        catalog.write_text('\n'.join(catalog.read_text().splitlines()[:5])+'\n')
        with ThreadPoolExecutor(max_workers=2) as pool:
            replies=list(pool.map(lambda action:self.get('/api/playback/'+action,{}),['channelUp','channelDown']))
        self.assertTrue(all(reply['playing'] for reply in replies));self.assertEqual(self.get('/api/state')['title'],'Station 0')
        catalog.write_text(catalog.read_text().replace('/1\n','/slow\n'))
        result=[]
        def changing():
            try: result.append(self.get('/api/playback/channelUp',{}))
            except urllib.error.HTTPError as error: result.append(error.code);error.close()
        pending=threading.Thread(target=changing);pending.start();time.sleep(.2)
        self.get('/api/playback/stop',{});pending.join(3);self.assertFalse(pending.is_alive());self.assertTrue(result and isinstance(result[0],int));self.assertFalse(self.get('/api/state')['playing'])
if __name__=='__main__':
    suite=unittest.TestSuite([Surf('test_adjacent_preserves_stream_on_failure_and_wraps'),Radio('test_radio_stream_and_fullscreen_transport')]);result=unittest.TextTestRunner(verbosity=2).run(suite);sys.exit(not result.wasSuccessful())
