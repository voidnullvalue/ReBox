#!/usr/bin/env python3
"""Run isolated HR54 remux playback tests and restore the prior IPTV channel."""
import importlib.util,json,pathlib,re,time,urllib.request,uuid
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'tests/youtube-20261001';HOST='192.168.88.103'
s=importlib.util.spec_from_file_location('deploy',ROOT/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
def api(route,body=None):
 req=urllib.request.Request('http://'+HOST+':8130'+route,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
 return json.load(urllib.request.urlopen(req,timeout=60))
def remote(cmd):return d.remote(HOST,cmd)
before=api('/api/status');assert before['receiverHost']==HOST
saved={p:api(p) for p in ['/api/tv/state','/api/iptv/state','/api/auth/status']};(OUT/'baseline.json').write_text(json.dumps({'status':before,'state':saved},indent=2))
try:
 if before['playing']:assert before['source']=='iptv';api('/api/iptv/stop',{});time.sleep(10)
 for mode,label in [(0,'local'),(1,'streamed')]:
  marker='YOUTUBE_REMUX_'+uuid.uuid4().hex
  r=remote('echo '+marker+' >> /var/viewer/messages.log\n/var/hr54-transfer/yt-stream-test '+str(mode)+' >/tmp/yt-'+label+'.log 2>&1 </dev/null &\necho TEST_SERVER_PID=$!')
  (OUT/(label+'-start.log')).write_text(r)
  pid=int(re.search(r'TEST_SERVER_PID=(\d+)',r)[1]);time.sleep(.5)
  print(label,remote('/var/opt/hr54/bin/hr54-play-url http://127.0.0.1:18100/test.ts'),flush=True)
  samples=[]
  for n in range(4):
   time.sleep(4);samples.append(remote('cat /tmp/yt-'+label+'.log; cat /proc/meminfo | head -5; for f in /proc/[0-9]*/status; do if grep -q "^Name:.*yt-remux" $f; then grep -E "^(Name|Pid|PPid|VmRSS|VmHWM):" $f; p=${f#/proc/}; p=${p%/status}; cat /proc/$p/stat; fi; done'))
  (OUT/(label+'-resources.json')).write_text(json.dumps(samples,indent=2))
  raw=remote('tail -n 3500 /var/viewer/messages.log');at=raw.find(marker+'\n');assert at>=0
  log=raw[at:].split('hr54-root#')[0];(OUT/(label+'-receiver.log')).write_text(log)
  (OUT/(label+'-worker.log')).write_text(remote('cat /tmp/yt-'+label+'.log'))
  print(label,{k:k in log for k in ['TransportType MPEG ','notifyStarted','notifyFramePresented#ENTER','audio type: AAC','AUDIO_PID AAC']},flush=True)
  urllib.request.urlopen('http://'+HOST+':8080/remote/processKey?hold=keyPress&key=stop',timeout=10).read();time.sleep(10)
finally:
 for p in ['/api/tv/state','/api/iptv/state','/api/auth/status']:assert api(p)==saved[p],p
 if before['playing']:print('RESTORE',api('/api/iptv/play',{'channelId':before['channelId']}),flush=True)
 (OUT/'restored-status.json').write_text(json.dumps(api('/api/status'),indent=2))
