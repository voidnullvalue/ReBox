#!/usr/bin/env python3
import importlib.util,json,pathlib,re,time,urllib.request,uuid
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'tests/youtube-20261001';HOST='192.168.88.103'
s=importlib.util.spec_from_file_location('d',ROOT/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
def api(p,b=None):return json.load(urllib.request.urlopen(urllib.request.Request('http://'+HOST+':8130'+p,data=None if b is None else json.dumps(b).encode(),headers={'Content-Type':'application/json'}),timeout=60))
def remote(c):return d.remote(HOST,c)
before=api('/api/status');assert before['receiverHost']==HOST
states={p:api(p) for p in ['/api/tv/state','/api/iptv/state','/api/auth/status']};marker='YOUTUBE_REAL_'+uuid.uuid4().hex
try:
 if before['playing']:assert before['source']=='iptv';api('/api/iptv/stop',{});time.sleep(10)
 remote('echo '+marker+' >> /var/viewer/messages.log')
 print(remote('PYTHONHOME=/var/hr54-transfer/yt-python-home /var/hr54-transfer/yt-python /var/hr54-transfer/yt-launch.py'),flush=True);time.sleep(1)
 print(remote('/var/opt/hr54/bin/hr54-play-url http://127.0.0.1:18100/test.ts'),flush=True)
 samples=[]
 for n in range(6):
  time.sleep(4);samples.append(remote('cat /tmp/yt-real-worker.log; cat /proc/meminfo | head -8; for f in /proc/[0-9]*/status; do if grep -q "^Name:.*yt-" $f; then grep -E "^(Name|Pid|PPid|VmRSS|VmHWM):" $f; p=${f#/proc/}; p=${p%/status}; cat /proc/$p/stat; fi; done'))
 (OUT/'real-play-resources.json').write_text(json.dumps(samples,indent=2));raw=remote('tail -3500 /var/viewer/messages.log');at=raw.find(marker+'\n');assert at>=0
 log=raw[at:].split('hr54-root#')[0];(OUT/'real-play-receiver.log').write_text(log);(OUT/'real-play-worker.log').write_text(remote('cat /tmp/yt-real-worker.log'))
 print({k:k in log for k in ['TransportType MPEG ','notifyStarted','notifyFramePresented#ENTER','audio type: AAC','AUDIO_PID AAC']},flush=True)
finally:
 urllib.request.urlopen('http://'+HOST+':8080/remote/processKey?hold=keyPress&key=stop',timeout=10).read();time.sleep(10)
 for p in states:assert api(p)==states[p],p
 if before['playing']:print('RESTORE',api('/api/iptv/play',{'channelId':before['channelId']}),flush=True)
