"""Local Frigate camera/configuration fixture; decoder remains host-mocked."""
import http.server
import base64
import hashlib
import json
import shutil
import struct
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
import runtime
ROOT=runtime.ROOT
MEDIA=None
def media():
    global MEDIA
    if MEDIA is None:
        MEDIA=subprocess.check_output(['ffmpeg','-v','error','-f','lavfi','-i','color=size=640x360:rate=25','-f','lavfi','-i','sine=sample_rate=48000','-t','1','-c:v','libx264','-preset','ultrafast','-pix_fmt','yuv420p','-c:a','aac','-movflags','empty_moov+frag_keyframe','-f','mp4','pipe:1'])
    return MEDIA
class Frigate(unittest.TestCase):
    start=runtime.Runtime.start;stop=runtime.Runtime.stop;get=runtime.Runtime.get
    def setUp(self):
        runtime.Runtime.setUp(self)
        self.frames=media();self.ws_requests=[];self.ws_mode='media';owner=self
        class Upstream(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                if self.path.startswith('/live/mse/api/ws?src=front_sub'):
                    owner.ws_requests.append(self.path)
                    if owner.ws_mode=='html':
                        self.send_response(200);self.send_header('Content-Type','text/html');self.end_headers();self.wfile.write(b'<html>Frigate app</html>');return
                    key=self.headers.get('Sec-WebSocket-Key','')
                    accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade');self.send_header('Sec-WebSocket-Accept',accept if owner.ws_mode!='bad_accept' else 'wrong');self.end_headers();self.wfile.flush()
                    def send(op,data,fin=True):
                        n=len(data);head=bytes([(128 if fin else 0)|op]);head+=bytes([n]) if n<126 else b'\x7e'+struct.pack('!H',n)
                        self.wfile.write(head+data);self.wfile.flush()
                    try:
                        h=self.rfile.read(2);n=h[1]&127;mask=self.rfile.read(4);data=self.rfile.read(n);request=bytes(b^mask[i%4] for i,b in enumerate(data))
                        if json.loads(request)!={'type':'mse','value':'avc1.640029,mp4a.40.2'}:return
                        if owner.ws_mode=='codec':send(1,b'{"type":"mse","value":"video/mp4; codecs=hvc1"}');return
                        send(1,b'{"type":"mse","value":"video/mp4; codecs=avc1.42c01e,mp4a.40.2"}')
                        send(9,b'ping')
                        # Binary fragments and an interleaved control frame.
                        middle=len(owner.frames)//2
                        send(2,owner.frames[:middle],False);send(10,b'pong');send(0,owner.frames[middle:])
                        time.sleep(2);send(8,b'')
                    except (BrokenPipeError,ConnectionResetError,IndexError):pass
                    return
                if self.path=='/api/config':
                    value={'cameras':{'Front':{'live':{'streams':{'main':'front_sub'}},'ffmpeg':{'inputs':[{'path':'rtsp://private:password@camera/live'}]}},'HEVC':{'live':{'streams':{'main':'hevc'}}},'Disabled':{'enabled':False}},'go2rtc':{'streams':{'front_sub':['rtsp://private:password@camera/live'],'hevc':['rtsp://private:password@camera/hevc']}}}
                elif self.path=='/api/go2rtc/streams/front_sub':value={'producers':[{'medias':['video, recvonly, H264']} ]}
                elif self.path=='/api/go2rtc/streams/hevc':value={'producers':[{'medias':['video, recvonly, H265']} ]}
                else:self.send_error(404);return
                raw=json.dumps(value).encode();self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
        self.upstream=http.server.ThreadingHTTPServer(('127.0.0.1',0),Upstream);threading.Thread(target=self.upstream.serve_forever,daemon=True).start();self.addCleanup(self.upstream.server_close);self.addCleanup(self.upstream.shutdown)
        module=self.root/'modules/frigate';(module/'bin').mkdir(parents=True);shutil.copy(ROOT/'../modules/frigate/module.json',module/'module.json');shutil.copy(ROOT/'build/frigate-module',module/'bin/module');shutil.copy(ROOT/'build/frigate-relay',module/'bin/relay')
        self.data=self.root/'module-data/frigate';self.data.mkdir(parents=True);(self.data/'config.json').write_text(json.dumps({'host':'127.0.0.1','port':self.upstream.server_port}))
        state=self.root/'module-state';state.mkdir();(state/'frigate.json').write_text('{"schema":1,"enabled":true}')
        self.start();self.prefix='/api/modules/frigate';self.master=(state/'management.secret').read_text()
    def authorized(self,body):
        request=urllib.request.Request(self.url+self.prefix+'/settings',data=json.dumps(body).encode(),headers={'Content-Type':'application/json','Authorization':'Bearer '+self.master})
        with urllib.request.urlopen(request) as response:return json.load(response)
    def test_cameras_are_generic_items_without_config_secrets(self):
        rows=self.get(self.prefix+'/browse')['items'];self.assertEqual([row['playable'] for row in rows],[True,False,False]);self.assertTrue(all(row['kind']=='item' for row in rows));self.assertNotIn('password',json.dumps(rows));self.assertNotIn('rtsp:',json.dumps(rows))
        self.assertEqual(self.get(self.prefix+'/search?q=ront')['items'][0]['id'],'Front')
        self.assertEqual(self.get(self.prefix+'/browse?limit=1&offset=1')['items'][0]['id'],'HEVC')
        legacy=self.get('/api/frigate/cameras');self.assertEqual(legacy['cameras'][0]['stream'],'front_sub');self.assertNotIn('password',json.dumps(legacy))
    def stream(self):
        token=self.get(self.prefix+'/status')['playback']['session']
        return urllib.request.urlopen(self.url+'/module-stream/frigate/'+token+'.ts',timeout=5)
    def test_generic_and_legacy_proxy_playback_and_stop(self):
        state=self.get(self.prefix+'/play',{'itemId':'Front'});self.assertTrue(state['playing']);self.assertEqual(state['source'],'frigate');self.assertTrue(state['live']);self.assertFalse(state['transport']['seek']);self.get('/api/frigate/stop',{});self.assertFalse(self.get('/api/state')['playing'])
        state=self.get('/api/frigate/play',{'cameraId':'Front'});self.assertTrue(state['playing'])
        with self.stream() as stream:self.assertEqual(stream.read(188)[0],0x47)
        self.assertEqual(self.ws_requests,['/live/mse/api/ws?src=front_sub']);self.get('/api/playback/stop',{})
        for id in ['HEVC','Disabled','http://arbitrary.example']:
            with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/play',{'itemId':id})
            self.assertEqual(error.exception.code,502);error.exception.close()
    def test_html_upgrade_bad_accept_and_wrong_codec_fail_without_media(self):
        for mode in ['html','bad_accept','codec']:
            self.ws_mode=mode;self.get(self.prefix+'/play',{'itemId':'Front'})
            with self.assertRaises(urllib.error.HTTPError) as error:self.stream()
            self.assertEqual(error.exception.code,502);error.exception.close()
            deadline=time.monotonic()+3
            while self.get('/api/state')['playing'] and time.monotonic()<deadline:time.sleep(.05)
            self.assertFalse(self.get('/api/state')['playing']);self.assertTrue(self.get('/api/system/status')['ready'])
        log=(self.root/'log/frigate.log').read_text();self.assertIn('HTTP status=200',log);self.assertIn('lacks H.264',log);self.assertNotIn('password',log)
    def test_packet_copy_preserves_video_audio_and_session_stop(self):
        self.get(self.prefix+'/play',{'itemId':'Front'})
        with self.stream() as stream:raw=stream.read()
        path=self.base/'camera.ts';path.write_bytes(raw)
        info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=codec_name','-of','json',str(path)]))
        self.assertEqual([s['codec_name'] for s in info['streams']],['h264','aac'])
        self.get(self.prefix+'/play',{'itemId':'Front'})
        token=self.get(self.prefix+'/status')['playback']['session']
        # A stop for an old session must not retire the new camera.
        import socket
        body=b'{"session":"stale"}'
        with socket.socket(socket.AF_UNIX) as rpc:
            rpc.connect(str(self.socketdir/'frigate.sock'));rpc.sendall(b'POST /playback/stop HTTP/1.0\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body);rpc.recv(4096)
        self.assertEqual(self.get(self.prefix+'/status')['playback']['session'],token);self.assertTrue(self.get('/api/state')['playing'])
        with self.stream() as stream:
            self.assertEqual(stream.read(188)[0],0x47);before=time.monotonic();self.get('/api/playback/stop',{});self.assertLess(time.monotonic()-before,1)
        self.assertFalse(self.get('/api/state')['playing'])
    def test_host_configuration_authorization_and_persistence(self):
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/settings',{'host':'127.0.0.2'})
        self.assertEqual(error.exception.code,401);error.exception.close()
        self.authorized({'host':'127.0.0.2'});self.stop();self.start();self.assertEqual(self.get(self.prefix+'/settings')['fields'][0]['value'],'127.0.0.2')
        for body in [{'host':'bad host'},{'port':0},{'port':70000},{'port':123.5},{'unknown':'x'}]:
            with self.assertRaises(urllib.error.HTTPError) as error:self.authorized(body)
            self.assertEqual(error.exception.code,400);error.exception.close()
        self.assertTrue(self.get('/api/system/status')['ready']);self.assertTrue(self.get('/api/modules')['modules'][0]['healthy'])
    def test_broken_upstream_does_not_break_core_or_module_health(self):
        self.authorized({'port':1});self.stop();self.start();self.assertTrue(self.get('/api/system/status')['ready']);self.assertTrue(self.get('/api/modules')['modules'][0]['healthy'])
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/browse')
        self.assertEqual(error.exception.code,502);error.exception.close()
if __name__=='__main__':unittest.main()
