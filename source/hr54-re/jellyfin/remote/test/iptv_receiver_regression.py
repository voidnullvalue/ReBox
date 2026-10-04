#!/usr/bin/env python3
"""Live regression of the preserved Media Hub, Frigate and Jellyfin paths."""
import importlib.util,json,pathlib,time,urllib.request,os
R=pathlib.Path(__file__).resolve().parents[1];O=pathlib.Path(os.environ.get('HR54_TEST_OUTPUT',str(R.parent/'tests/iptv-20261001')));HOST='192.168.88.103';B='http://'+HOST+':8130'
sp=importlib.util.spec_from_file_location('d',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
def api(p,body=None):return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=90))
def key(k,delay=1):urllib.request.urlopen('http://'+HOST+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10).read();time.sleep(delay)
def remote(s):return d.remote(HOST,s)
assert not api('/api/status')['playing'];ip_before=api('/api/iptv/state');jf_before=api('/api/tv/state');assert api('/api/auth/status')['authenticated']
key('menu',24);key('down');key('select',8)
cams=api('/api/frigate/cameras');assert any(c['id']=='camera_07c3' and c['playable'] for c in cams['cameras']);(O/'frigate-cameras.json').write_text(json.dumps(cams,indent=2))
api('/api/frigate/play',{'cameraId':'camera_07c3'});time.sleep(12);status=api('/api/status');assert status['playing'] and status['source']=='frigate' and status['live'];(O/'frigate-status.json').write_text(json.dumps(status,indent=2));print('PASS Frigate discovery and preserved direct H.264 camera playback',flush=True)
(O/'frigate.receiver.log').write_text(remote('tail -1800 /var/viewer/messages.log'))
api('/api/transport',{'action':'stop'});time.sleep(12);assert not api('/api/status')['playing'];ret=remote('tail -35 /var/hr54-persist/jellyfin/log/tv-events.log');assert 'source-frigate' in ret;(O/'frigate-return.log').write_text(ret)
key('exit');key('up');key('select',4);assert api('/api/auth/status')['authenticated'];assert api('/api/tv/state')==jf_before;assert api('/api/iptv/state')==ip_before
items=api('/api/items?videoOnly=1&limit=6&offset=0')['items'];movie=next(c for c in items if c['playable'])
api('/api/play',{'itemId':movie['id'],'returnToTv':True});time.sleep(8);assert api('/api/status')['source']=='jellyfin'
api('/api/transport',{'action':'pause'});assert api('/api/status')['paused'];api('/api/transport',{'action':'resume'});assert not api('/api/status')['paused'];api('/api/seek',{'delta':5});time.sleep(4);assert api('/api/status')['source']=='jellyfin';api('/api/transport',{'action':'stop'});time.sleep(12)
ret=remote('tail -40 /var/hr54-persist/jellyfin/log/tv-events.log');assert 'source-jellyfin' in ret;(O/'jellyfin-return.log').write_text(ret)
assert api('/api/tv/state')==jf_before and api('/api/iptv/state')==ip_before;assert api('/api/auth/status')['authenticated']
print('PASS Jellyfin auth, browse state, native playback, pause, resume, seek, STOP and owner return; isolated IPTV state',flush=True)
for _ in range(12):
 result=remote('/opt/middleware_core/system/tv/uconntest \'<com.ucentric.pvruconnect.DirectTest command="itvGetAppStatus" sessionId="0"/>\'')
 if 'No app running' in result:break
 key('exit')
assert 'No app running' in result;(O/'media-exit.log').write_text(result);api('/api/tv/state',jf_before)
assert not api('/api/status')['playing'] and not api('/api/iptv/status')['active'];print('PASS MEDIA BACK/EXIT leaves stock receiver idle',flush=True)
