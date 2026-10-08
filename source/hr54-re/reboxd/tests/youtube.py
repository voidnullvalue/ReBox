"""Generic search-first module with real resolver process and stub CDN/remux."""
import json
import os
import py_compile
import shutil
import socket
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
import runtime
ROOT=runtime.ROOT
class Youtube(unittest.TestCase):
    start=runtime.Runtime.start; stop=runtime.Runtime.stop; get=runtime.Runtime.get
    def setUp(self):
        runtime.Runtime.setUp(self)
        module=self.root/'modules/youtube';(module/'bin').mkdir(parents=True)
        shutil.copy(ROOT/'../modules/youtube/module.json',module/'module.json');shutil.copy(ROOT/'build/youtube-module',module/'bin/module')
        self.data=self.root/'module-data/youtube';y=self.data/'youtube';(y/'bin').mkdir(parents=True)
        (y/'bin/python').write_text('#!/bin/sh\nunset PYTHONHOME\nexec '+sys.executable+' "$@"\n');(y/'bin/python').chmod(0o700)
        src=self.base/'resolver.py';src.write_text('''import sys,json,time
mode,arg,page=sys.argv[1:]
if arg in ('slow','slow0000000'):time.sleep(30)
if mode=='search':print(json.dumps({'page':int(page),'hasMore':int(page)<2,'results':[{'id':'jNQXAC9IVRw','title':'Example video','channel':'A channel','duration':2}]}))
else:print(json.dumps({'videoUrl':'https://cdn.example/video?signature=private-test-secret','audioUrl':'https://cdn.example/audio?signature=private-test-secret','title':'Example video','duration':2}))
''');py_compile.compile(str(src),cfile=str(y/'resolver.pyc'))
        # Relay fixture emits TS then stays alive to exercise concurrent Stop.
        relay=y/'bin/relay'
        relay.write_text('#!/bin/sh\nunset PYTHONHOME\nexec '+sys.executable+' "$0.worker"\n');relay.chmod(0o700)
        (y/'bin/relay.worker').write_text('import os,time\nos.write(1, b"G"+b"x"*187)\ntime.sleep(30)\n')
        state=self.root/'module-state';state.mkdir();(state/'youtube.json').write_text('{"schema":1,"enabled":true}')
        self.start();self.prefix='/api/modules/youtube';self.master=(state/'management.secret').read_text()
    def test_search_first_and_legacy_state(self):
        module=self.get('/api/modules')['modules'][0];self.assertFalse(module['capabilities']['browse']);self.assertTrue(module['capabilities']['search'])
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/browse')
        self.assertEqual(error.exception.code,404);error.exception.close()
        row=self.get(self.prefix+'/search?q=example')['items'][0];self.assertEqual(row['title'],'Example video');self.assertEqual(row['subtitle'],'A channel');self.assertTrue(row['playable'])
        self.assertEqual(self.get('/api/youtube/search?q=test&page=2')['page'],2)
        self.get('/api/youtube/state',{'query':'test','page':3,'index':4,'videoId':'jNQXAC9IVRw'})
        self.stop();self.start();self.assertEqual(self.get('/api/youtube/state')['query'],'test')
    def test_play_proxy_transport_and_invalid_id(self):
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/play',{'itemId':'https://arbitrary.example'})
        self.assertEqual(error.exception.code,502);error.exception.close()
        state=self.get('/api/youtube/play',{'videoId':'jNQXAC9IVRw'});self.assertEqual(state['source'],'youtube');self.assertFalse(state['live']);self.assertFalse(state['transport']['seek'])
        token=self.get(self.prefix+'/status')['playback']['session']
        stream=urllib.request.urlopen(self.url+'/module-stream/youtube/'+token+'.ts',timeout=5);self.addCleanup(stream.close);self.assertEqual(stream.read(188)[0],0x47)
        self.assertTrue(self.get('/api/system/status')['ready']);self.get('/api/youtube/stop',{});self.assertFalse(self.get('/api/state')['playing'])
        log=(self.root/'log/youtube.log').read_text();self.assertIn('YouTube selected video URL=https://cdn.example/[redacted]',log);self.assertNotIn('private-test-secret',log);self.assertIn('resolver exit=0',log)
    def test_cancel_search_and_play_preparation(self):
        for path,body in [('/api/youtube/search?q=slow',None),(self.prefix+'/play',{'itemId':'slow0000000'})]:
            result=[]
            def starting():
                try:self.get(path,body)
                except urllib.error.HTTPError as error:result.append(error.code);error.close()
            worker=threading.Thread(target=starting);worker.start();time.sleep(.2)
            before=time.monotonic();self.get('/api/youtube/stop',{});worker.join(3)
            self.assertLess(time.monotonic()-before,1.5);self.assertFalse(worker.is_alive());self.assertEqual(result,[502])
            self.assertTrue(self.get('/api/system/status')['ready']);self.assertFalse(self.get('/api/state')['playing'])
    def test_settings_authorization_and_persistence(self):
        path=self.prefix+'/settings'
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(path, {'automaticUpdates':False})
        self.assertEqual(error.exception.code,401);error.exception.close()
        request=urllib.request.Request(self.url+path,data=b'{"automaticUpdates":false}',headers={'Content-Type':'application/json','Authorization':'Bearer '+self.master})
        with urllib.request.urlopen(request) as response:self.assertFalse(json.load(response)['fields'][0]['value'])
        self.stop();self.start();self.assertFalse(self.get(path)['fields'][0]['value'])

    def test_slow_decoder_backpressure_preserves_stream(self):
        size=8*1024*1024
        worker=self.data/'youtube/bin/relay.worker'
        worker.write_text('import os\nos.write(1,b"G"+b"x"*'+str(size-1)+')\n')
        self.get(self.prefix+'/play',{'itemId':'jNQXAC9IVRw'})
        token=self.get(self.prefix+'/status')['playback']['session']
        stream=urllib.request.urlopen(self.url+'/module-stream/youtube/'+token+'.ts',timeout=10)
        self.addCleanup(stream.close);self.assertEqual(stream.read(188)[0],0x47)
        time.sleep(6)
        self.assertEqual(len(stream.read()),size-188)
        self.assertTrue(self.get('/api/state')['playing'])
        self.get('/api/playback/stop',{})

    def test_late_eof_keeps_decoder_drain_and_remains_cancellable(self):
        # Delivery ends after the old duration-based deadline has expired.
        worker=self.data/'youtube/bin/relay.worker'
        worker.write_text('import os,time\nos.write(1,b"G"+b"x"*187)\ntime.sleep(21)\n')
        self.get(self.prefix+'/play',{'itemId':'jNQXAC9IVRw'})
        token=self.get(self.prefix+'/status')['playback']['session']
        stream=urllib.request.urlopen(self.url+'/module-stream/youtube/'+token+'.ts',timeout=30)
        self.addCleanup(stream.close);self.assertEqual(stream.read(188)[0],0x47)
        self.assertEqual(stream.read(1),b'')
        self.assertTrue(self.get(self.prefix+'/status')['playback']['playing'])
        self.assertTrue(self.get('/api/state')['playing'])
        before=time.monotonic();self.get('/api/playback/stop',{})
        self.assertLess(time.monotonic()-before,1.5);self.assertFalse(self.get('/api/state')['playing'])

    def test_missing_resolver_does_not_break_readiness(self):
        self.stop();(self.data/'youtube/resolver.pyc').unlink();self.start();self.assertTrue(self.get('/api/modules')['modules'][0]['healthy']);self.assertTrue(self.get('/api/system/status')['ready'])
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'/search?q=test')
        self.assertEqual(error.exception.code,502);error.exception.close()
if __name__=='__main__':unittest.main()
