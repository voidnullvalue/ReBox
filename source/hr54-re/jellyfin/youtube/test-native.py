#!/usr/bin/env python3
"""Exercise deployed ID-only API and physical STOP/owner return on receiver."""
import importlib.util,pathlib,urllib.request,json,time,uuid
R=pathlib.Path(__file__).resolve().parents[1];O=R/'tests/youtube-20261001';H='192.168.88.103';B='http://'+H+':8130'
s=importlib.util.spec_from_file_location('deploy',R/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
def api(p,b=None):
 try:return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if b is None else json.dumps(b).encode(),headers={'Content-Type':'application/json'}),timeout=90))
 except urllib.error.HTTPError as e:
  print('API ERROR',p,e.code,e.read().decode(),flush=True);raise
def key(k):urllib.request.urlopen('http://'+H+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10).read()
assert not api('/api/status')['playing'];
for _ in range(30):
 if not api('/api/youtube/status')['active']:break
 time.sleep(1)
saved={p:api(p) for p in ['/api/tv/state','/api/iptv/state','/api/auth/status']}
api('/api/youtube/state',{'query':'me at the zoo','page':0,'index':0,'videoId':'jNQXAC9IVRw'})
mark='NATIVE_YOUTUBE_'+uuid.uuid4().hex;d.remote(H,'echo '+mark+' >> /var/viewer/messages.log')
start=time.monotonic();result=api('/api/youtube/play',{'videoId':'jNQXAC9IVRw'});print('PLAY',result,'elapsed',round(time.monotonic()-start,3),flush=True)
samples=[]
for i in range(3):
 time.sleep(3);samples.append({'status':api('/api/status'),'processes':d.remote(H,'for f in /proc/[0-9]*/status; do if grep -q "^Name:.*\\(python\\|qjs\\|relay\\|remux\\|hr54-iptv-fetch\\)" $f; then grep -E "^(Name|Pid|PPid|VmRSS|VmHWM):" $f; p=${f#/proc/}; p=${p%/status}; cat /proc/$p/stat; fi; done')})
key('stop');time.sleep(12);assert not api('/api/status')['playing'];
# Returning frontend re-runs its ephemeral result search on-box.
for _ in range(30):
 if not api('/api/youtube/status')['active']:break
 time.sleep(1)
assert not api('/api/youtube/status')['active'];assert api('/api/youtube/state')['query']=='me at the zoo'
ret=d.remote(H,'tail -50 /var/hr54-persist/jellyfin/log/tv-events.log');(O/'native-youtube-return.log').write_text(ret);assert 'source-youtube' in ret
log=d.remote(H,'tail -4500 /var/viewer/messages.log');at=log.find(mark+'\n');assert at>=0;log=log[at:];(O/'native-youtube.receiver.log').write_text(log)
checks={k:k in log for k in ['TransportType MPEG ','audio type: AAC','AUDIO_PID AAC','notifyStarted','notifyFramePresented#ENTER']};assert all(checks.values()),checks
(O/'native-youtube-resources.json').write_text(json.dumps(samples,indent=2))
for p,v in saved.items():assert api(p)==v,p
left=d.remote(H,'ps | grep -E "youtube/bin/(python|qjs|relay|remux)" | grep -v grep; ls -d /tmp/hr54-youtube-* 2>/dev/null');(O/'native-youtube-cleanup.log').write_text(left)
print('PASS native YouTube A/V, physical STOP, source return, saved search and existing state isolation',checks,flush=True)
