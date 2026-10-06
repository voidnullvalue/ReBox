import json, pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Registry(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=pathlib.Path(self.tmp.name)
    def run_registry(self,*args):
        p=subprocess.run([str(ROOT/'build/registry'),str(self.root),*args],capture_output=True,text=True,check=True)
        return json.loads(p.stdout)['modules']
    def install(self,id='unseen-provider',**fields):
        m=dict(schema=1,id=id,name='Provider',version='1.0',moduleApi=1,minReboxApi=1,kind='media',entrypoint='bin/module',capabilities={'browse':True})
        m.update(fields);d=self.root/'modules'/id;d.mkdir(parents=True,exist_ok=True);(d/'bin').mkdir(exist_ok=True)
        (d/'bin/module').write_text('executable fixture');(d/'bin/module').chmod(0o700);(d/'module.json').write_text(json.dumps(m));return d
    def test_pairing_expiration(self):
        self.run_registry()
        subprocess.run([str(ROOT/'build/test-auth'),str(self.root)],check=True,capture_output=True)
    def test_empty(self):self.assertEqual(self.run_registry(),[])
    def test_discovery_state_and_permissions(self):
        self.install();m=self.run_registry()[0];self.assertFalse(m['core']);self.assertFalse(m['enabled']);self.assertTrue(m['compatible'])
        self.run_registry('unseen-provider','1');self.assertTrue(self.run_registry()[0]['enabled'])
        self.assertEqual((self.root/'module-state/unseen-provider.json').stat().st_mode&0o777,0o600)
        self.run_registry('unseen-provider','0');self.assertFalse(self.run_registry()[0]['enabled'])
    def test_untrusted_core(self):
        self.install(core=True);self.assertFalse(self.run_registry()[0]['core'])
    def test_bad_modules_do_not_break_registry(self):
        for field,value in [('schema',2),('moduleApi',9),('name','x'*128),('entrypoint','../outside')]:
            self.install(**{field:value});m=self.run_registry()[0];self.assertFalse(m['compatible']);self.assertFalse(m['enabled']);self.assertTrue(m['error'])
    def test_symlink(self):
        d=self.install();(d/'bin/module').unlink();(d/'bin/module').symlink_to('/bin/true');self.assertFalse(self.run_registry()[0]['compatible'])
    def test_catalog_missing_core_and_trust(self):
        self.run_registry();(self.root/'builtin/catalog.json').write_text(json.dumps({'schema':1,'modules':[{'id':'bundled','name':'Bundled','package':'bundled.rbox','sha256':'a'*64,'defaultEnabled':True}]}))
        m=self.run_registry()[0];self.assertTrue(m['core']);self.assertFalse(m['installed']);self.assertFalse(m['enabled'])
        self.install('bundled');m=self.run_registry()[0];self.assertTrue(m['enabled']);self.assertTrue(m['core'])
    def test_duplicate_and_depth(self):
        d=self.install()
        for raw in ['{"id":"x","id":"y"}', '['*40+']'*40,'{}','{"id":"x\\u0000"}']:
            (d/'module.json').write_text(raw);self.assertFalse(self.run_registry()[0]['compatible'])
    def test_limit(self):
        for n in range(35):self.install('m'+str(n))
        self.assertEqual(len(self.run_registry()),32)
if __name__=='__main__':unittest.main()
