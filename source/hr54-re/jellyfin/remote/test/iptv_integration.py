#!/usr/bin/env python3
"""Controlled native HLS integration: redirects, headers, sequence, refresh, EOF,
concurrent control requests, malformed formats and cleanup. No internet probes."""
import collections,http.server,json,os,pathlib,subprocess,tempfile,threading,time,urllib.request
R=pathlib.Path(__file__).resolve().parents[1]
requests=[];refresh=collections.Counter()
ts=(R.parent/'tests/codec-20260930/media/hls-h264-aac/seg000.ts').read_bytes()
class Fixture(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  requests.append((self.path,self.headers.get('User-Agent'),self.headers.get('Referer')))
  if self.path=='/slow':time.sleep(2);self.send_error(503);return
  if self.path=='/start':self.send_response(302);self.send_header('Location','/a/master.m3u8?token=Q');self.end_headers();return
  if 'master.m3u8' in self.path:
   data=b'#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",URI="audio.m3u8"\n#EXT-X-STREAM-INF:BANDWIDTH=26000000,CODECS="avc1.640028"\nbig.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=4000000,CODECS="avc1.640028,mp4a.40.2"\n../low/live.m3u8?token=Q\n'
  elif 'live.m3u8' in self.path:
   refresh['live']+=1;start=min(10+max(0,refresh['live']-2),15)
   data=('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:'+str(start)+'\n'+''.join('#EXTINF:1,\nseg'+str(i)+'.ts?token=Q\n' for i in range(start,start+2))).encode()
  elif '/vod.m3u8' in self.path:data=b'#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:20\n#EXTINF:1,\nseg20.ts?token=Q\n#EXT-X-ENDLIST\n'
  elif '/fmp4' in self.path:data=b'#EXTM3U\n#EXT-X-MAP:URI="init.mp4"\n#EXTINF:1,\na.m4s\n'
  elif '/encrypted' in self.path:data=b'#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI="key"\n#EXTINF:1,\na.ts\n'
  elif '.ts' in self.path:data=ts
  else:self.send_error(404);return
  self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture);port=server.server_port
threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='hr54-iptv-') as tmp:
 root=pathlib.Path(tmp);persist=root/'persist';(persist/'iptv').mkdir(parents=True)
 playlist='#EXTM3U\n'+''.join(f'#EXTINF:-1 tvg-id="{name}" group-title="Test" http-user-agent="attribute, UA",{name}\n#EXTVLCOPT:http-user-agent=Exact, UA\n#EXTVLCOPT:http-referrer=https://ref.example/a?q=1,2\n{('srt://example.invalid:9999' if name=='SRT' else 'http://127.0.0.1:'+str(port)+'/'+url)}\n' for name,url in [('Live','start'),('VOD','vod.m3u8'),('fMP4','fmp4'),('Encrypted','encrypted'),('DASH','x.mpd'),('SRT','srt'),('Slow','slow')])
 (persist/'iptv/eng.m3u').write_text(playlist)
 env=dict(os.environ,JF_PERSIST_ROOT=str(persist),IPTV_FETCH_CMD='/tmp/hr54-iptv-fetch-host',JF_PLAY_CMD='sh '+str(R/'test/mock_playurl.sh'),MOCK_FETCH_OUT=str(root/'fetch.ts'),MOCK_FETCH_PROGRESS=str(root/'fetch.progress'))
 subprocess.run(['cc','-std=c11','-O1','-o',str(root/'native'),str(R/'hr54_jf.c')],check=True)
 with open(root/'native.log','w') as log:
  backend=subprocess.Popen([str(root/'native'),str(R/'static'),'127.0.0.1','18096','18135','--no-launcher'],env=env,stdout=log,stderr=log,start_new_session=True)
  def api(path,body=None):
   req=urllib.request.Request('http://127.0.0.1:18135'+path,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
   try:return json.load(urllib.request.urlopen(req,timeout=25))
   except urllib.error.HTTPError as e:return json.load(e)
  try:
   time.sleep(.5);channels={c['name']:c['id'] for c in api('/api/iptv/channels?limit=60')['channels']};assert len(channels)==7
   assert api('/api/iptv/play',{'url':'http://evil'})['error']=='Unknown IPTV channel ID'
   response=api('/api/iptv/play',{'channelId':channels['Live']});print(response,flush=True);assert response.get('source')=='iptv',response
   time.sleep(2);assert api('/api/state')['source']=='iptv';assert not api('/api/state')['transport']['seek'];assert api('/api/playback/pause',{})['ok'] is False;assert api('/api/playback/resume',{})['ok'] is False;assert api('/api/playback/seek',{'seconds':5})['ok'] is False;assert api('/api/transport',{'action':'pause'})['ok'] is False;assert api('/api/status')['source']=='iptv';assert api('/api/iptv/status')['workerPid']>0
   segs=[p for p,_,_ in requests if '.ts?' in p];assert len(segs)>=3 and len(set(segs))==len(segs),segs
   assert all(ua=='Exact, UA' and ref=='https://ref.example/a?q=1,2' for _,ua,ref in requests)
   assert '/a/master.m3u8?token=Q' in [p for p,_,_ in requests];assert not any('big.m3u8' in p or 'audio.m3u8' in p for p,_,_ in requests)
   assert api('/api/iptv/stop',{})['stopped'];time.sleep(.5);before=len(requests);time.sleep(.7);assert len(requests)==before;assert not api('/api/status')['playing'];assert not api('/api/iptv/status')['active']
   # Clean VOD EOF automatically tears down state.
   assert api('/api/iptv/play',{'channelId':channels['VOD']}).get('playing') or not api('/api/status')['playing'];time.sleep(.5);assert not api('/api/status')['playing'];assert 'Stream ended' in api('/api/iptv/status')['error']
   for name,reason in [('fMP4','fMP4'),('Encrypted','Encrypted'),('DASH','DASH'),('SRT','SRT')]:assert reason in api('/api/iptv/play',{'channelId':channels[name]})['error'];assert not api('/api/iptv/status')['active']
   result=[]
   starting=threading.Thread(target=lambda:result.append(api('/api/iptv/play',{'channelId':channels['Slow']})))
   starting.start();time.sleep(.2);assert api('/api/iptv/status')['active'];assert 'Stop current playback' in api('/api/iptv/play',{'channelId':channels['Live']})['error'];assert api('/api/iptv/stop',{})['stopped'];starting.join(3);assert not starting.is_alive() and 'error' in result[0];assert not api('/api/iptv/status')['active']
   assert api('/api/iptv/state',{'mode':'list','view':'group','group':'Sports','page':3,'index':4,'query':'ABC'})['saved'];state=api('/api/iptv/state');assert state['group']=='Sports' and state['page']==3 and state['index']==4
   (persist/'iptv/eng.m3u').write_text(playlist.replace(',Live\n',',Renamed\n'));assert 'Renamed' in [x['name'] for x in api('/api/iptv/channels?limit=60')['channels']]
   print('PASS native live HLS: redirect, relative master, headers, bandwidth selection, sequence deduplication, refresh, concurrent API, stop, VOD EOF, unsupported formats, ID enforcement, mtime reload, concurrent session rejection, preparation cancellation, independent native checkpoint')
  finally:
   import signal
   print((root/'native.log').read_text(),flush=True)
   os.killpg(backend.pid,signal.SIGTERM);backend.wait()
server.shutdown()
