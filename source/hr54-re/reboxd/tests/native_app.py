"""Unknown native apps and Doom lifecycle with a mock engine, never EGL."""
import json
import os
import pathlib
import secrets
import shutil
import subprocess
import time
import unittest
import urllib.error
import urllib.request
import runtime
ROOT=runtime.ROOT
class Native(unittest.TestCase):
    start=runtime.Runtime.start;stop=runtime.Runtime.stop;get=runtime.Runtime.get
    def setUp(self):runtime.Runtime.setUp(self)
    def install_native(self,id):
        d=self.root/'modules'/id;(d/'bin').mkdir(parents=True)
        manifest=json.loads((ROOT/'../modules/test-module/module.json').read_text());manifest.update(id=id,kind='native-app',capabilities={'nativeApp':True},presentation={'releaseInput':True,'releaseSurface':True})
        (d/'module.json').write_text(json.dumps(manifest));shutil.copy(ROOT/'build/test-module',d/'bin/module')
        state=self.root/'module-state';state.mkdir(exist_ok=True);(state/(id+'.json')).write_text('{"schema":1,"enabled":true}')
    def mutation(self,id):
        secret=(self.root/'module-state/management.secret').read_text();request=urllib.request.Request(self.url+'/api/modules/'+id+'/disable',data=b'{}',headers={'Authorization':'Bearer '+secret})
        with urllib.request.urlopen(request) as response:return json.load(response)
    def test_unknown_native_handoff_arbitration_and_restart(self):
        identity='future-native-'+secrets.token_hex(3);self.install_native(identity);self.install_native('second-native');runtime.Runtime.install(self,'media');self.start();prefix='/api/modules/'+identity+'/native/'
        self.assertTrue(self.get(prefix+'start',{})['running']);status=self.get('/api/system/status');self.assertEqual(status['nativeModule'],identity);self.assertTrue(status['mediaBusy'])
        self.assertTrue(self.get(prefix+'start',{})['running'])
        for path,body in [('/api/modules/second-native/native/start',{}),('/api/modules/media/play',{'itemId':'item-1'})]:
            with self.assertRaises(urllib.error.HTTPError) as error:self.get(path,body)
            self.assertEqual(error.exception.code,409);error.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as error:self.mutation(identity)
        self.assertEqual(error.exception.code,409);error.exception.close()
        self.get(prefix+'stop',{});self.assertFalse(self.get('/api/system/status')['mediaBusy'])
        self.get('/api/modules/media/play',{'itemId':'item-1'})
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(prefix+'start',{})
        self.assertEqual(error.exception.code,409);error.exception.close();self.get('/api/playback/stop',{})
        self.get(prefix+'start',{});journal=json.loads((self.root/'module-state/.native-owner.json').read_text());self.assertEqual(journal['module'],identity)
        self.stop();self.start()
        # Fresh mock module reports no retained presentation, releasing the journal.
        end=time.monotonic()+4
        while self.get('/api/system/status')['mediaBusy'] and time.monotonic()<end:time.sleep(.1)
        self.assertFalse(self.get('/api/system/status')['mediaBusy'])

class Doom(unittest.TestCase):
    start=runtime.Runtime.start;stop=runtime.Runtime.stop;get=runtime.Runtime.get
    def setUp(self):
        Native.setUp(self);d=self.root/'modules/doom';(d/'bin').mkdir(parents=True)
        shutil.copy(ROOT/'../modules/doom/module.json',d/'module.json');shutil.copy(ROOT/'build/doom-module',d/'bin/module')
        state=self.root/'module-state';state.mkdir();(state/'doom.json').write_text('{"schema":1,"enabled":true}')
        self.data=self.root/'module-data/doom/doom';(self.data/'data').mkdir(parents=True);(self.data/'data/doom1.wad').write_bytes(b'fixture')
        source=self.base/'engine.c';source.write_text('''#include <unistd.h>
#include <stdlib.h>
#include <string.h>
int main(int n,char **a){for(int i=1;i+1<n;i++)if(!strcmp(a[i],"--ready-fd")){char ready=1;int fd=atoi(a[i+1]);write(fd,&ready,1);close(fd);}for(;;)pause();}
''');subprocess.run(['cc','-o',str(d/'bin/hr54-doom-native'),str(source)],check=True)
        self.start();self.prefix='/api/modules/doom/native/'
        self.addCleanup(self.clean_engine)
    def clean_engine(self):
        if self.daemon and self.daemon.poll() is None:
            try:self.get(self.prefix+'stop',{})
            except OSError:pass
    def test_real_process_lock_identity_clean_stop_and_legacy_routes(self):
        response=self.get('/api/doom/start',{});self.assertTrue(response['running']);pid=response['pid'];self.assertGreater(pid,1)
        self.assertTrue(self.get('/api/doom/status')['running']);self.assertEqual(self.get('/api/system/status')['nativeModule'],'doom')
        self.assertTrue(self.get(self.prefix+'start',{})['running']);self.get('/api/doom/input',{'keys':17});self.assertEqual(self.get('/api/doom/input')['keys'],17)
        self.get('/api/doom/stop',{});self.assertFalse(self.get('/api/doom/status')['running']);self.assertFalse(self.get('/api/system/status')['mediaBusy'])
        end=time.monotonic()+2
        while pathlib.Path('/proc/'+str(pid)).exists() and time.monotonic()<end:time.sleep(.05)
        self.assertFalse(pathlib.Path('/proc/'+str(pid)).exists())
        (self.data/'doom.pid').write_text(str(self.daemon.pid));self.assertFalse(self.get('/api/doom/status')['running']);self.get('/api/doom/stop',{});self.assertIsNone(self.daemon.poll())
    def test_module_crash_preserves_native_ownership_and_rejoins(self):
        response=self.get(self.prefix+'start',{});engine=response['pid']
        children=runtime.module_children(self.daemon)
        self.assertEqual(len(children),1);os.kill(int(children[0]),9)
        time.sleep(.4);self.assertEqual(self.get('/api/system/status')['nativeModule'],'doom');self.assertTrue(self.get('/api/system/status')['mediaBusy'])
        end=time.monotonic()+6
        while time.monotonic()<end:
            if self.get('/api/modules')['modules'][0]['healthy']:break
            time.sleep(.1)
        self.assertTrue(self.get('/api/modules')['modules'][0]['healthy']);self.assertEqual(self.get('/api/doom/status')['pid'],engine)
        self.get(self.prefix+'stop',{});self.assertFalse(self.get('/api/system/status')['mediaBusy'])

    def test_missing_assets_keep_core_ready(self):
        (self.data/'data/doom1.wad').unlink();self.assertTrue(self.get('/api/system/status')['ready'])
        with self.assertRaises(urllib.error.HTTPError) as error:self.get(self.prefix+'start',{})
        self.assertEqual(error.exception.code,502);error.exception.close()
        end=time.monotonic()+3
        while self.get('/api/system/status')['mediaBusy'] and time.monotonic()<end:time.sleep(.1)
        self.assertFalse(self.get('/api/system/status')['mediaBusy'])
if __name__=='__main__':unittest.main()
