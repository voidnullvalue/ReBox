#!/usr/bin/env python3
"""Bounded native worker integration, cancellation and state isolation."""
import pathlib,tempfile,subprocess,os,json,urllib.request,urllib.error,time,threading,py_compile
R=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hr54-youtube-test-') as td:
 t=pathlib.Path(td);p=t/'persist';y=p/'youtube';(y/'bin').mkdir(parents=True);(t/'www').mkdir()
 (y/'bin/python').write_text('#!/bin/sh\nunset PYTHONHOME\nexec '+os.sys.executable+' "$@"\n');(y/'bin/python').chmod(0o700)
 src=t/'resolver.py';src.write_text('import sys,json,time\nif sys.argv[2]=="slow":time.sleep(30)\nprint(json.dumps({"page":int(sys.argv[3]),"hasMore":False,"results":[{"id":"jNQXAC9IVRw","title":"Test"}]}))\n')
 py_compile.compile(str(src),cfile=str(y/'resolver.pyc'))
 subprocess.run(['cc','-std=c11','-O1','-o',str(t/'backend'),str(R/'hr54_jf.c')],check=True)
 env=dict(os.environ,JF_PERSIST_ROOT=str(p));proc=subprocess.Popen([str(t/'backend'),str(t/'www'),'127.0.0.1','18096','18132','--no-launcher'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 def api(route,data=None):
  r=urllib.request.Request('http://127.0.0.1:18132'+route,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
  try:
   with urllib.request.urlopen(r,timeout=35) as f:return f.status,json.load(f)
  except urllib.error.HTTPError as e:return e.code,json.load(e)
 try:
  time.sleep(.5);assert api('/api/youtube/status')[1]['available']
  assert api('/api/youtube/play',{'videoId':'https://arbitrary.example'})[1]['error']=='Invalid video ID'
  saved={'query':'test','page':3,'index':4,'videoId':'jNQXAC9IVRw'};assert api('/api/youtube/state',saved)[0]==200;assert api('/api/youtube/state')[1]==dict(saved,ok=True)
  jf=api('/api/tv/state')[1];response=[];th=threading.Thread(target=lambda:response.append(api('/api/youtube/search?q=slow&page=0')));th.start()
  for i in range(20):
   if api('/api/youtube/status')[1]['active']:break
   time.sleep(.1)
  begin=time.monotonic();assert api('/api/status')[0]==200;assert time.monotonic()-begin<1
  assert api('/api/youtube/search?q=other&page=0')[0]!=200
  api('/api/youtube/stop',{});th.join(3);assert not th.is_alive();assert 'cancelled' in response[0][1]['error']
  assert not api('/api/youtube/status')[1]['active'];assert api('/api/youtube/search?q=ok&page=2')[1]['page']==2
  assert api('/api/youtube/state')[1]==dict(saved,ok=True);assert api('/api/tv/state')[1]==jf
  time.sleep(.2)
  for f in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
   try:cmd=f.read_bytes()
   except OSError:continue
   assert str(y/'resolver.pyc').encode() not in cmd,'orphan resolver'
  print('PASS ID validation, no arbitrary proxy, responsive main service, worker exclusivity, cancellation, exact teardown and independent saved state')
 finally:proc.terminate();proc.wait(timeout=5)
