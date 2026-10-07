import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Defaults(unittest.TestCase):
    def test_atomic_defaults_preserve_private_data_and_reject_links(self):
        with tempfile.TemporaryDirectory() as d:
            root=pathlib.Path(d);source=root/'s';data=root/'d';source.mkdir();data.mkdir()
            (source/'config').write_text('distribution-default');(data/'config').write_text('existing-account')
            (source/'bin').mkdir();(source/'bin/helper').write_bytes(b'executable');(source/'bin/helper').chmod(0o700)
            def seed():return subprocess.run([str(ROOT/'build/test-seed'),str(source),str(data)]).returncode
            self.assertEqual(seed(),0);self.assertEqual((data/'config').read_text(),'existing-account');self.assertEqual((data/'bin/helper').stat().st_mode&0o777,0o700)
            self.assertEqual(seed(),0);self.assertEqual(list(data.rglob('*.seed-*')),[])
            (source/'escape').symlink_to(root/'outside');self.assertNotEqual(seed(),0);(source/'escape').unlink()
            (data/'bin/helper').unlink();(data/'bin/helper').symlink_to(root/'outside');self.assertNotEqual(seed(),0);self.assertFalse((root/'outside').exists())
if __name__=='__main__':unittest.main()
