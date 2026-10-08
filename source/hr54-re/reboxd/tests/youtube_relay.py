"""Exercise the production FIFO relay with bounded synthetic CDN responses."""
import hashlib,json,os,pathlib,signal,subprocess,sys,tempfile,time,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Relay(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.work=pathlib.Path(self.temp.name);(self.work/'bin').mkdir()
  self.binary=self.work/'relay'
  subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread',str(ROOT/'../modules/youtube/relay.c'),'-o',str(self.binary)],check=True)
  fetch=self.work/'fetch';fetch.write_text('#!'+sys.executable+'\n'+r'''import os,sys,json,time,urllib.parse,pathlib
u=urllib.parse.urlsplit(sys.argv[1]);q=urllib.parse.parse_qs(u.query);kind=u.path.strip('/');size=int(q.get('clen',['2621563' if kind=='video' else '65573'])[0])
if q.get('slow'):time.sleep(30)
first,last=(map(int,q['range'][0].split('-')) if 'range' in q else (0,size-1))
with open(os.environ['TEST_RANGES'],'a') as log:log.write(json.dumps([kind,first,last])+'\n')
body=bytes(i%251 for i in range(first,last+1))
if q.get('short'):body=body[:-1]
sys.stdout.buffer.write((sys.argv[1]+'\nvideo/mp4\n\n').encode());sys.stdout.buffer.write(body)
''');fetch.chmod(0o700)
  mux=self.work/'bin/remux';mux.write_text('#!'+sys.executable+'\n'+r'''import sys,os,json,hashlib
v=open(sys.argv[1],'rb').read();a=open(sys.argv[2],'rb').read()
open(os.environ['TEST_RESULT'],'w').write(json.dumps([[len(v),hashlib.sha256(v).hexdigest()],[len(a),hashlib.sha256(a).hexdigest()]]))
os.write(1,b'G'+b'x'*187)
''');mux.chmod(0o700)
  self.env=dict(os.environ,REBOX_YOUTUBE_FETCH=str(fetch),REBOX_YOUTUBE_RUNTIME=str(self.work),TEST_RESULT=str(self.work/'result.json'),TEST_RANGES=str(self.work/'ranges.jsonl'))
 def run_relay(self,host='mock.googlevideo.com',extra=''):
  return subprocess.run([str(self.binary),f'https://{host}/video?clen=2621563{extra}',f'https://{host}/audio?clen=65573'],env=self.env,capture_output=True,timeout=15)
 def test_chunked_prefetch_preserves_both_tracks(self):
  result=self.run_relay();self.assertEqual(result.returncode,0,result.stderr.decode());self.assertEqual(result.stdout,b'G'+b'x'*187)
  tracks=json.loads((self.work/'result.json').read_text())
  for track,size in zip(tracks,[2621563,65573]):self.assertEqual(track,[size,hashlib.sha256(bytes(i%251 for i in range(size))).hexdigest()])
  requests=[json.loads(line) for line in (self.work/'ranges.jsonl').read_text().splitlines()]
  for name,size in [('video',2621563),('audio',65573)]:
   offset=0
   for _,first,last in sorted((r for r in requests if r[0]==name),key=lambda r:r[1]):self.assertEqual(first,offset);self.assertLessEqual(last-first+1,1048576);offset=last+1
   self.assertEqual(offset,size)
  self.assertNotIn('https://',result.stderr.decode())
 def test_unknown_cdn_keeps_whole_file_fallback(self):
  result=self.run_relay('cdn.example');self.assertEqual(result.returncode,0,result.stderr.decode());requests=[json.loads(line) for line in (self.work/'ranges.jsonl').read_text().splitlines()];self.assertEqual(len(requests),2)
 def test_short_range_fails_instead_of_skipping_bytes(self):
  result=self.run_relay(extra='&short=1');self.assertNotEqual(result.returncode,0)
 def test_cancel_while_prefilling_is_bounded(self):
  p=subprocess.Popen([str(self.binary),'https://mock.googlevideo.com/video?clen=2621563&slow=1','https://mock.googlevideo.com/audio?clen=65573'],env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
  time.sleep(.2);start=time.monotonic();os.killpg(p.pid,signal.SIGTERM);p.communicate(timeout=3);self.assertLess(time.monotonic()-start,2)
if __name__=='__main__':unittest.main()
