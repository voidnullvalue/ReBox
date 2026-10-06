import functools,gzip,hashlib,http.server,io,json,pathlib,shutil,tarfile,threading,time,unittest,urllib.request,urllib.error
import runtime
ROOT=runtime.ROOT
class Installer(runtime.Runtime):
    def setUp(self):
        super().setUp();self.downloads=self.base/'downloads';self.downloads.mkdir()
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def log_message(self,*args):pass
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(self.downloads)))
        threading.Thread(target=self.server.serve_forever,daemon=True).start();self.addCleanup(self.server.server_close);self.addCleanup(self.server.shutdown);self.token=None
    def req(self,path,body=None,method=None,token=None):
        headers={'Content-Type':'application/json'}
        if token is not None:headers['Authorization']='Bearer '+token
        req=urllib.request.Request(self.url+path,data=json.dumps(body).encode() if body is not None else None,headers=headers,method=method)
        try:
            with urllib.request.urlopen(req,timeout=15) as r:return r.status,json.load(r)
        except urllib.error.HTTPError as e:
            with e:return e.code,json.load(e)
    def auth(self):
        master=(self.root/'module-state/management.secret').read_text();code,data=self.req('/api/management/pair/open',{},token=master);self.assertEqual(code,200)
        code,data=self.req('/api/management/pair',{'code':data['code']});self.assertEqual(code,200);self.token=data['token'];return self.token
    def archive(self,name='sample.rbox',manifest=None,extra=(),executable=True):
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text())
        if manifest is not None:m=manifest
        with tarfile.open(self.downloads/name,'w:gz',format=tarfile.USTAR_FORMAT) as tar:
            entries=[]
            if m is not False:entries.append(('module.json',json.dumps(m).encode(),0o600,'0'))
            if executable:entries.append(('bin/module',(ROOT/'build/test-module').read_bytes(),0o700,'0'))
            for path,raw,mode,kind in entries+list(extra):
                info=tarfile.TarInfo(path);info.size=len(raw);info.mode=mode;info.type=kind.encode()
                if kind=='2':info.linkname='../../escape'
                tar.addfile(info,io.BytesIO(raw))
        return f'http://127.0.0.1:{self.server.server_port}/{name}'
    def install_url(self,url,sha=None):
        body={'url':url}
        if sha is not None:body['sha256']=sha
        return self.req('/api/modules/install',body,token=self.token)
    def test_authorization(self):
        self.start()
        for path,method in [('/api/modules/install','POST'),('/api/modules/any/enable','POST'),('/api/modules/any/disable','POST'),('/api/modules/any','DELETE')]:
            self.assertEqual(self.req(path,{},method)[0],401)
            self.assertEqual(self.req(path,{},method,'0'*64)[0],401)
        master=(self.root/'module-state/management.secret').read_text();code,d=self.req('/api/management/pair/open',{},token=master);self.assertEqual(code,200);pair=d['code']
        self.assertEqual(self.req('/api/management/pair',{'code':pair})[0],200)
        self.assertEqual(self.req('/api/management/pair',{'code':pair})[0],403)
        self.assertEqual((self.root/'module-state/management.secret').stat().st_mode&0o777,0o600)
    def test_url_install_disable_restart_enable_remove(self):
        self.start();self.auth();url=self.archive();code,data=self.install_url(url);self.assertEqual(code,200,data)
        self.assertTrue(self.get('/api/modules')['modules'][0]['healthy'])
        self.assertEqual(self.req('/api/modules/test-media/disable',{},token=self.token)[0],200)
        self.stop();self.start();self.assertFalse(self.get('/api/modules')['modules'][0]['enabled'])
        self.assertEqual(self.req('/api/modules/test-media/enable',{},token=self.token)[0],200)
        self.get('/api/modules/test-media/play',{'itemId':'item-1'})
        self.assertEqual(self.req('/api/modules/test-media/disable',{},token=self.token)[0],409)
        self.get('/api/playback/stop',{})
        self.assertEqual(self.req('/api/modules/test-media',{},'DELETE',self.token)[0],200)
        self.assertEqual(self.get('/api/modules')['modules'],[])
        self.assertTrue((self.root/'module-data/test-media').is_dir())
    def test_archive_rejections(self):
        self.start();self.auth()
        for path,kind in [('../escape','0'),('/absolute','0'),('link','2'),('device','3'),('pipe','6'),('module.json','0')]:
            with self.subTest(path=path):self.assertEqual(self.install_url(self.archive(extra=[(path,b'',0o600,kind)]))[0],400)
        self.assertEqual(self.install_url(self.archive(manifest=False))[0],400)
        self.assertEqual(self.install_url(self.archive(executable=False))[0],400)
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text())
        for key,val in [('id','INVALID'),('id','x'*65),('schema',2),('moduleApi',2),('entrypoint','../bin/module')]:
            with self.subTest(key=key,val=val):self.assertEqual(self.install_url(self.archive(manifest={**m,key:val}))[0],400)
        self.assertEqual(self.install_url(self.archive(),'0'*64)[0],400)
        self.assertEqual(self.get('/api/modules')['modules'],[])
    def test_invalid_icon_bounds(self):
        self.start();self.auth()
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text());m['icon']='icon.png'
        for raw in [b'not png',b'\x89PNG\r\n\x1a\n'+(13).to_bytes(4,'big')+b'IHDR'+(513).to_bytes(4,'big')+(128).to_bytes(4,'big')+b'\0'*20]:
            self.assertEqual(self.install_url(self.archive(manifest=m,extra=[('icon.png',raw,0o600,'0')]))[0],400)
    def test_failed_update_rolls_back(self):
        self.start();self.auth();self.assertEqual(self.install_url(self.archive())[0],200)
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text());m['version']='2.0';m['entrypoint']='bin/broken'
        url=self.archive(manifest=m,extra=[('bin/broken',b'not an executable',0o700,'0')]);self.assertEqual(self.install_url(url)[0],502)
        m=self.get('/api/modules')['modules'][0];self.assertEqual(m['version'],'1.0.0');self.assertTrue(m['healthy'])
        self.assertEqual(self.get('/api/modules/test-media/browse')['total'],2)
    def test_builtin_offline_reinstall(self):
        url=self.archive();raw=(self.downloads/'sample.rbox').read_bytes();digest=hashlib.sha256(raw).hexdigest()
        store=self.root/'builtin';store.mkdir();(store/'sample.rbox').write_bytes(raw)
        (store/'catalog.json').write_text(json.dumps({'schema':1,'modules':[{'id':'test-media','name':'Test media','package':'sample.rbox','sha256':digest,'defaultEnabled':True}]}))
        self.start();self.auth();self.assertFalse(self.get('/api/modules')['modules'][0]['installed'])
        code,d=self.req('/api/modules/test-media/reinstall',{},token=self.token);self.assertEqual(code,200,d)
        self.assertTrue(self.get('/api/modules')['modules'][0]['core'])
        self.assertEqual(self.req('/api/modules/test-media',{},'DELETE',self.token)[0],200)
        m=self.get('/api/modules')['modules'][0];self.assertTrue(m['core']);self.assertFalse(m['installed'])
        self.assertTrue((store/'sample.rbox').exists())
        self.assertEqual(self.req('/api/modules/test-media/reinstall',{},token=self.token)[0],200)
    def test_interrupted_upgrade_recovery(self):
        self.install();self.start();self.stop()
        stage='a'*32;d=self.root/'staging'/stage;d.mkdir();live=self.root/'modules/test-media'
        shutil.copy(self.root/'module-state/test-media.json',d/'state.old');live.rename(d/'old');live.mkdir();(live/'module.json').write_text('broken')
        (self.root/'module-state/transaction.json').write_text(json.dumps({'id':'test-media','stage':stage,'hadOld':True,'hadState':True}))
        self.start();self.assertTrue(self.get('/api/modules')['modules'][0]['healthy']);self.assertFalse(d.exists())
    def test_upgrade(self):
        self.start();self.auth();self.assertEqual(self.install_url(self.archive())[0],200)
        m=json.loads((ROOT/'../modules/test-module/module.json').read_text());m['version']='2.0';self.assertEqual(self.install_url(self.archive(manifest=m))[0],200)
        self.assertEqual(self.get('/api/modules')['modules'][0]['version'],'2.0')
if __name__=='__main__':unittest.main()
