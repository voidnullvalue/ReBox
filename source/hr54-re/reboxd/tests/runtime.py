import json,os,pathlib,shutil,socket,subprocess,tempfile,time,unittest,urllib.request,urllib.error
ROOT=pathlib.Path(__file__).resolve().parents[1]
def module_children(process):
    children=set()
    for task in pathlib.Path(f'/proc/{process.pid}/task').iterdir():
        try:children.update((task/'children').read_text().split())
        except FileNotFoundError:pass  # A completed HTTP worker can exit between reads.
    return sorted(children)

class Runtime(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='rb-');self.addCleanup(self.tmp.cleanup);self.base=pathlib.Path(self.tmp.name);self.root=self.base/'r';self.root.mkdir()
        self.socketdir=self.base/'s';self.daemon=None
    def install(self,id='test-media'):
        d=self.root/'modules'/id;(d/'bin').mkdir(parents=True)
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text());m['id']=id;(d/'module.json').write_text(json.dumps(m));shutil.copy(ROOT/'build/test-module',d/'bin/module')
        state=self.root/'module-state';state.mkdir(exist_ok=True);(state/f'{id}.json').write_text('{"schema":1,"enabled":true}')
    def start(self):
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        self.url=f'http://127.0.0.1:{port}';self.daemon=subprocess.Popen([str(ROOT/'build/reboxd-host'),str(self.root),str(port),str(self.socketdir)],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)
        for _ in range(150):
            try:
                modules=self.get('/api/modules')['modules']
                if all(not m['enabled'] or m['healthy'] for m in modules):return
                time.sleep(.05)
            except OSError:time.sleep(.05)
        raise AssertionError('core did not start')
    def stop(self):
        if self.daemon and self.daemon.poll() is None:self.daemon.terminate();self.daemon.wait(timeout=10)
    def get(self,path,body=None):
        req=urllib.request.Request(self.url+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=10) as r:return json.load(r)
    def test_zero(self):self.start();self.assertEqual(self.get('/api/modules')['modules'],[]);self.assertTrue(self.get('/api/system/status')['ready'])
    def test_unknown_id_browse_search_play(self):
        id='not-in-core-742';self.install(id);self.start();m=self.get('/api/modules')['modules'][0];self.assertTrue(m['healthy'])
        prefix='/api/modules/'+id;root=self.get(prefix+'/browse');self.assertEqual(root['items'][0]['kind'],'folder')
        self.assertEqual(self.get(prefix+'/browse?parent=folder-a')['items'][0]['id'],'item-2')
        self.assertEqual(self.get(prefix+'/search?q=foo')['items'][0]['id'],'item-1')
        p=self.get(prefix+'/play',{'itemId':'item-1'});self.assertTrue(p['playing']);self.assertEqual(p['source'],id);self.assertEqual(p['generation'],1)
        self.get('/api/playback/stop',{});self.assertFalse(self.get('/api/state')['playing'])
        self.stop();self.assertEqual(list(self.socketdir.glob('*.sock')),[])
    def test_direct_http_plans_are_generic_and_validated(self):
        self.install('future-direct');self.start();prefix='/api/modules/future-direct/play'
        for url in ['http://127.0.0.1:9999/live.ts?x=a%26b','https://media.example/live','http://[::1]:80/live']:
            state=self.get(prefix,{'itemId':'opaque','testDirectUrl':url});self.assertTrue(state['playing']);self.assertEqual(state['source'],'future-direct');self.get('/api/playback/stop',{})
        for url in ['file:///etc/passwd','http://user:secret@host/live','http://host:99999/live','http://host:bad/live','http://host/\r\nInjected: x','http:///missing','http://bad host/live','http://[broken]/live']:
            with self.assertRaises(urllib.error.HTTPError) as error:self.get(prefix,{'itemId':'opaque','testDirectUrl':url})
            self.assertEqual(error.exception.code,502);error.exception.close();self.assertFalse(self.get('/api/state')['playing'])

    def test_active_module_crash_clears_playback(self):
        self.install();self.start();self.get('/api/modules/test-media/play',{'itemId':'item-1'})
        children=module_children(self.daemon)
        self.assertEqual(len(children),1);os.kill(int(children[0]),9)
        for _ in range(30):
            if not self.get('/api/state')['playing']:break
            time.sleep(.05)
        self.assertFalse(self.get('/api/state')['playing']);self.assertTrue(self.get('/api/system/status')['ready'])
    def test_crash_containment(self):
        self.install();self.start();children=module_children(self.daemon)
        self.assertEqual(len(children),1);os.kill(int(children[0]),9)
        time.sleep(.3);self.assertTrue(self.get('/api/system/status')['ready']);self.assertFalse(self.get('/api/modules')['modules'][0]['healthy'])
if __name__=='__main__':unittest.main()
