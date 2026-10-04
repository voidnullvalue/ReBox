#!/usr/bin/env python3
"""Isolated HTTP contract checks; no receiver or Internet access."""
import http.server,json,os,pathlib,signal,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error
R=pathlib.Path(__file__).resolve().parents[1]
class Frigate(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def do_GET(self):
  data={'cameras':{'driveway':{'enabled':True}},'go2rtc':{'streams':{'driveway':'rtsp://user:SECRET@camera'}}} if self.path=='/api/config' else {'producers':[{'medias':['video, recvonly, H264']}]} 
  raw=json.dumps(data).encode();self.send_response(200);self.end_headers();self.wfile.write(raw)
f=http.server.ThreadingHTTPServer(('127.0.0.1',0),Frigate)
threading.Thread(target=f.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='hr54-api-') as td:
 t=pathlib.Path(td);p=t/'persist';(p/'iptv').mkdir(parents=True)
 (p/'iptv/eng.m3u').write_text('#EXTM3U\n#EXTINF:-1 tvg-id="news" tvg-logo="https://img.example/news.png" group-title="News",News One\nhttp://example.invalid/news.ts\n#EXTINF:-1 group-title="Sports",Sports One\nhttp://example.invalid/sport.ts\n')
 subprocess.run(['cc','-std=c11','-O1','-DFRIGATE_HOST="127.0.0.1"',f'-DFRIGATE_PORT={f.server_port}','-o',str(t/'backend'),str(R/'hr54_jf.c')],check=True)
 sock=socket.socket();sock.bind(('127.0.0.1',0));port=sock.getsockname()[1];sock.close()
 env=dict(os.environ,JF_PERSIST_ROOT=str(p));proc=subprocess.Popen([str(t/'backend'),str(R/'static'),'127.0.0.1','18096',str(port),'--no-launcher'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
 def api(path,body=None,method=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=None if body is None else json.dumps(body).encode(),method=method,headers={'Content-Type':'application/json'})
  try:
   with urllib.request.urlopen(req,timeout=5) as r:return r.status,json.load(r)
  except urllib.error.HTTPError as e:return e.code,json.load(e)
 try:
  time.sleep(.5)
  code,c=api('/api/capabilities');assert code==200 and c['ok'] and all(c[k] for k in ['jellyfin','iptv','youtube','frigate','playback']);assert not c['doom'] and c['settingsWritable'] and c['settings']
  code,s=api('/api/state');assert s['ok'] and not s['playing'] and s['source'] is None and s['duration'] is None and not s['transport']['seek']
  assert api('/api/jellyfin/status')[1]['configured'];assert not api('/api/jellyfin/status')[1]['authenticated']
  assert api('/api/settings')[1]['writableKeys']==['jellyfinVideoBitrate'];assert api('/api/settings',{})[0]==400
  assert api('/api/settings')[1]['settings']['jellyfinVideoBitrate']==12000000
  for value in [0,-1,50000000,16000001,12000000.5,'16000000',None,True,{},[],1e300]:
   assert api('/api/settings',{'jellyfinVideoBitrate':value})[0]==400
   assert api('/api/settings')[1]['settings']['jellyfinVideoBitrate']==12000000
  assert api('/api/settings',{'jellyfinVideoBitrate':16000000,'server':'evil'})[0]==400
  for rate in [8000000,12000000,16000000]:
   code,d=api('/api/settings',{'jellyfinVideoBitrate':rate});assert code==200 and d['settings']['jellyfinVideoBitrate']==rate
   assert json.loads((p/'config/playback-quality.json').read_text())=={'jellyfinVideoBitrate':rate}
   assert (p/'config/playback-quality.json').stat().st_mode & 0o777 == 0o600
  rows=api('/api/iptv/channels?limit=1')[1];assert rows['hasMore'] and rows['offset']==0 and rows['channels'][0]['logo'].endswith('news.png')
  rows=api('/api/iptv/channels?query=Sports')[1];assert rows['total']==1 and rows['channels'][0]['group']=='Sports';assert api('/api/iptv/groups')[1]['total']==2
  for route in ['/api/frigate/cameras','/api/frigate/status']:
   code,c=api(route);assert code==200 and c['cameras'][0]['id']=='driveway' and c['cameras'][0]['playable'];assert 'SECRET' not in json.dumps(c)
  assert not api('/api/youtube/status')[1]['available'];assert api('/api/youtube/search?q=test')[1]['ok'] is False
  assert not api('/api/doom/status')[1]['running'];assert api('/api/doom/start',{})[0]==409
  for route,body,expected in [('/api/play',{'itemId':'../../etc/passwd'},400),('/api/youtube/play',{'videoId':'bad'},400),('/api/iptv/play',{'channelId':'bad'},404),('/api/frigate/play',{'cameraId':'http://evil'},400),('/api/transport',{'action':'bogus'},400),('/api/playback/seek',{'seconds':'oops'},400),('/api/playback/seek',{'seconds':1e300},400),('/api/playback/seek',{},400),('/api/playback/pause',{},409),('/api/playback/resume',{},409),('/api/playback/stop',{},409),('/api/playback/seek',{'seconds':10},409),('/api/play',[],400)]:
   code,d=api(route,body);assert code==expected and d['ok'] is False and d['error'],(route,code,d)
  assert api('/api/state',{},method='PUT')[0]==405
  with urllib.request.urlopen(f'http://127.0.0.1:{port}/tv/app.js') as r:assert r.read()==(R/'static/tv/app.js').read_bytes()
  print('PASS discovery, aggregate idle state, configuration redaction, IPTV metadata/search/paging, Frigate discovery/codecs/redaction, unavailable YouTube, Doom excluded, transport conflicts, invalid IDs/actions/bodies, methods, unchanged TV assets')
 finally:os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=5)
f.shutdown()
