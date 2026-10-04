#!/usr/bin/env python3
"""Real receiver UI -> selected supplied HTTPS channel -> hardware evidence."""
import importlib.util,json,pathlib,re,time,urllib.request,uuid
R=pathlib.Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('deploy',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
HOST='192.168.88.103';B='http://'+HOST+':8130';O=R.parent/'tests/iptv-20261001'
def api(p,body=None):return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=90))
def key(k,delay=.35):urllib.request.urlopen('http://'+HOST+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10).read();time.sleep(delay)
def remote(s):return d.remote(HOST,s)
assert api('/api/status')['receiverHost']==HOST and not api('/api/status')['playing']
before=api('/api/tv/state');auth=api('/api/auth/status');assert auth['authenticated']
api('/api/iptv/state',{'mode':'root','view':'all','page':0,'index':0,'rootPage':0,'rootIndex':0,'query':'','submitted':''});api('/api/tv/exit',{});key('menu',24);key('down');key('down');key('select',2);key('select')
# Shared keyboard types JAZ to find the supplied Al Jazeera entry.
for k in ['down','right','right','select','up','left','left','select','down','down','down','right','right','right','right','select','down','down','select']:key(k)
time.sleep(2)
marker='IPTV_REAL_'+uuid.uuid4().hex
remote('echo '+marker+' >> /var/viewer/messages.log')
key('select',8)
status=api('/api/status');print('PLAY',status,flush=True);assert status.get('source')=='iptv' and status['playing'] and status['channelName']=='Al Jazeera English (1080p)',status
samples=[]
for i in range(3):
 time.sleep(6);samples.append({'status':api('/api/status'),'iptv':api('/api/iptv/status'),'resources':remote('date -u; cat /proc/meminfo | head -5; cat /proc/loadavg; for f in /proc/[0-9]*/status; do if grep -q "^Name:.*hr54-" $f; then echo $f; grep -E "^(Name|Pid|PPid|VmRSS|VmSize|Threads):" $f; p=${f#/proc/}; p=${p%/status}; cat /proc/$p/stat; fi; done')});print('CONTINUING',samples[-1]['status'],flush=True)
assert all(s['status']['playing'] and s['status']['source']=='iptv' for s in samples)
assert samples[-1]['status']['elapsed']>samples[0]['status']['elapsed']
(O/'real-https-status-resources.json').write_text(json.dumps(samples,indent=2))
raw=remote('tail -n 3000 /var/viewer/messages.log');at=raw.find(marker+'\n');assert at>=0,'Evidence marker rotated out';log=raw[at:].split('hr54-root#')[0];(O/'real-https.receiver.log').write_text(log)
assert '/iptv-stream/' in log
assert 'TransportType MPEG ' in log
assert 'configured decodecontext inputchannel' in log
assert 'notifyFramePresented#ENTER' in log
assert 'audio type: AAC' in log and 'AUDIO_PID AAC' in log, 'Al Jazeera muxed AAC must configure hardware audio'
assert len(re.findall('position updated to',log))>=5
native=remote('tail -80 /var/hr54-persist/jellyfin/log/jf.log');(O/'real-https.native.log').write_text(native);assert 'IPTV segment seq=' in native and 'bandwidth 6128079' in native and 'H.264 video PID' in native
print('PASS UI search selected channel, local TS fetched, H.264 recognized, decoder configured, frame presented, continuing positions',flush=True)
key('stop',12)
assert not api('/api/status')['playing'] and not api('/api/iptv/status')['active']
ret=remote('tail -45 /var/hr54-persist/jellyfin/log/tv-events.log; for f in /proc/[0-9]*/status; do if grep -q "^Name:.*hr54-iptv" $f; then cat $f | head -6; fi; done');(O/'stop-return.log').write_text(ret);assert 'source-iptv' in ret
assert api('/api/iptv/state')['submitted']=='JAZ';assert api('/api/tv/state')==before;assert api('/api/auth/status')['authenticated']
# Selecting from the restored results must play the same channel, not MEDIA.
key('select',1)
for attempt in range(30):
 if api('/api/status').get('channelId')==status['channelId']:break
 time.sleep(1)
assert api('/api/status').get('channelId')==status['channelId'];key('exit',12);assert not api('/api/status')['playing']
key('exit');key('exit');key('exit') # results -> root -> MEDIA -> stock
(O/'final-jellyfin-state.json').write_text(json.dumps(api('/api/tv/state'),indent=2))
print('PASS STOP returns to IPTV search page with previous focus; reselect and EXIT stop; independent Jellyfin auth/state; IPTV root and MEDIA BACK',flush=True)
