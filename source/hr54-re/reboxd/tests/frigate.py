"""Local Frigate camera/configuration fixture; decoder remains host-mocked."""
import http.server
import json
import shutil
import threading
import unittest
import urllib.error
import urllib.request
import runtime
ROOT=runtime.ROOT
class Frigate(unittest.TestCase):
    start=runtime.Runtime.start;stop=runtime.Runtime.stop;get=runtime.Runtime.get
    def setUp(self):
        runtime.Runtime.setUp(self)
        class Upstream(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                if self.path=='/api/config':
                    value={'cameras':{'Front':{'live':{'streams':{'main':'front_sub'}},'ffmpeg':{'inputs':[{'path':'rtsp://private:password@camera/live'}]}},'HEVC':{'live':{'streams':{'main':'hevc'}}},'Disabled':{'enabled':False}},'go2rtc':{'streams':{'front_sub':['rtsp://private:password@camera/live'],'hevc':['rtsp://private:password@camera/hevc']}}}
                elif self.path=='/api/go2rtc/streams/front_sub':value={'producers':[{'medias':['video, recvonly, H264']} ]}
                elif self.path=='/api/go2rtc/streams/hevc':value={'producers':[{'medias':['video, recvonly, H265']} ]}
                else:self.send_error(404);return
                raw=json.dumps(value).encode();self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
        self.upstream=http.server.ThreadingHTTPServer(('127.0.0.1',0),Upstream);threading.Thread(target=self.upstream.serve_forever,daemon=True).start();self.addCleanup(self.upstream.server_close);self.addCleanup(self.upstream.shutdown)
        module=self.root/'modules/frigate';(module/'bin').mkdir(parents=True);shutil.copy(ROOT/'../modules/frigate/module.json',module/'module.json');shutil.copy(ROOT/'build/frigate-module',module/'bin/module')
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
    def test_generic_and_legacy_direct_playback_and_stop(self):
        state=self.get(self.prefix+'/play',{'itemId':'Front'});self.assertTrue(state['playing']);self.assertEqual(state['source'],'frigate');self.assertTrue(state['live']);self.assertFalse(state['transport']['seek']);self.get('/api/frigate/stop',{});self.assertFalse(self.get('/api/state')['playing'])
        state=self.get('/api/frigate/play',{'cameraId':'Front'});self.assertTrue(state['playing']);self.get('/api/playback/stop',{})
        for id in ['HEVC','Disabled','http://arbitrary.example']:
            with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/play',{'itemId':id})
            self.assertEqual(error.exception.code,502);error.exception.close()
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
