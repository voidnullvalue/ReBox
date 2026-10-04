#!/usr/bin/env python3
"""Post-YouTube live IPTV HTTPS/HLS regression without changing saved state."""
import json,urllib.request,time,pathlib,uuid,importlib.util,re,os
R=pathlib.Path(__file__).resolve().parents[1];O=pathlib.Path(os.environ.get('HR54_TEST_OUTPUT',str(R/'tests/youtube-20261001')));H='192.168.88.103';B='http://'+H+':8130'
s=importlib.util.spec_from_file_location('d',R/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
def api(p,b=None):return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if b is None else json.dumps(b).encode(),headers={'Content-Type':'application/json'}),timeout=90))
assert not api('/api/status')['playing'];states={p:api(p) for p in ['/api/tv/state','/api/iptv/state','/api/auth/status','/api/youtube/state']}
g=api('/api/iptv/groups');assert g['groups'];c=api('/api/iptv/channels?query=JAZ&limit=60')['channels'];selected=next(x for x in c if x['name']=='Al Jazeera English (1080p)');iptv_expected=dict(states['/api/iptv/state'],mode='list',view='search',query='JAZ',submitted='JAZ',page=0,index=0,channelId=selected['id']);api('/api/iptv/state',iptv_expected);mark='PRESERVED_IPTV_'+uuid.uuid4().hex;d.remote(H,'echo '+mark+' >> /var/viewer/messages.log')
try:
 result=api('/api/iptv/play',{'channelId':selected['id']});assert result['playing'];samples=[]
 for i in range(3):time.sleep(6);samples.append(api('/api/status'))
 assert samples[-1]['elapsed']>samples[0]['elapsed'];assert all(x['source']=='iptv' and x['playing'] for x in samples)
 log=d.remote(H,'tail -3500 /var/viewer/messages.log');at=log.find(mark+'\n');assert at>=0;log=log[at:];(O/'post-reboot-iptv.receiver.log').write_text(log)
 for x in ['/iptv-stream/','TransportType MPEG ','configured decodecontext inputchannel','notifyStarted','notifyFramePresented#ENTER','audio type: AAC','AUDIO_PID AAC']:assert x in log,x
 assert len(re.findall('position updated to',log))>=5
 (O/'post-reboot-iptv.status.json').write_text(json.dumps(samples,indent=2));(O/'post-reboot-iptv.native.log').write_text(d.remote(H,'tail -80 /var/hr54-persist/jellyfin/log/jf.log'))
 print('PASS existing IPTV groups/search, verified HTTPS HLS, H.264 hardware frames, AAC configuration and advancing position',flush=True)
finally:api('/api/iptv/stop',{});time.sleep(12)
try:
 assert api('/api/iptv/state')==iptv_expected
 for p,v in states.items():
  if p!='/api/iptv/state':assert api(p)==v,p
finally:
 api('/api/tv/exit',{});api('/api/iptv/state',states['/api/iptv/state'])
assert api('/api/iptv/state')==states['/api/iptv/state']
assert not api('/api/iptv/status')['active'];ret=d.remote(H,'tail -40 /var/hr54-persist/jellyfin/log/tv-events.log');assert 'source-iptv' in ret;(O/'post-reboot-iptv.return.log').write_text(ret)
print('PASS IPTV STOP owner return and all independent checkpoints preserved',flush=True)
