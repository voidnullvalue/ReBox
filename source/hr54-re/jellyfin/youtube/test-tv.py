#!/usr/bin/env python3
"""Physical MENU -> fourth source -> remote keyboard -> search -> playback."""
import importlib.util,pathlib,json,urllib.request,time,uuid,os
R=pathlib.Path(__file__).resolve().parents[1];O=pathlib.Path(os.environ.get('HR54_TEST_OUTPUT',str(R/'tests/youtube-20261001')));H='192.168.88.103';B='http://'+H+':8130'
s=importlib.util.spec_from_file_location('d',R/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
def api(p,b=None):return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if b is None else json.dumps(b).encode(),headers={'Content-Type':'application/json'}),timeout=90))
def key(k,delay=.35):urllib.request.urlopen('http://'+H+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10).read();time.sleep(delay)
def idle():
 for _ in range(60):
  if not api('/api/youtube/status')['active']:return
  time.sleep(1)
 raise AssertionError('YouTube did not become idle')
def enter():
 api('/api/tv/exit',{});key('menu',24)
 for _ in range(3):key('down')
 key('select',3)
assert not api('/api/status')['playing'];idle();saved={p:api(p) for p in ['/api/tv/state','/api/iptv/state','/api/auth/status']}
api('/api/youtube/state',{'query':'','page':0,'index':0,'videoId':''});enter();key('select',1)
# Z=row3,col4; O=row2,col0; SEARCH=row5,col4 in shared remote keyboard.
for k in ['down','down','down','right','right','right','right','select','up','left','left','left','left','select','select','down','down','down','right','right','right','right','select']:key(k)
time.sleep(2);idle();state=api('/api/youtube/state');assert state['query']=='ZOO',state
(O/'post-reboot-youtube-keyboard-state.json').write_text(json.dumps(state,indent=2));print('PASS physical MENU fourth source and remote keyboard receiver-native search',flush=True)
key('exit');key('exit');key('exit');api('/api/youtube/state',{'query':'me at the zoo','page':0,'index':0,'videoId':'jNQXAC9IVRw'});enter();idle()
mark='YOUTUBE_TV_'+uuid.uuid4().hex;d.remote(H,'echo '+mark+' >> /var/viewer/messages.log');key('select',1)
for _ in range(60):
 status=api('/api/status')
 if status['playing']:break
 time.sleep(1)
assert status['playing'] and status['source']=='youtube';time.sleep(8)
log=d.remote(H,'tail -4000 /var/viewer/messages.log');at=log.find(mark+'\n');assert at>=0;log=log[at:];(O/'post-reboot-youtube.receiver.log').write_text(log)
for x in ['TransportType MPEG ','configured decodecontext inputchannel','notifyStarted','notifyFramePresented#ENTER','audio type: AAC','AUDIO_PID AAC']:assert x in log,x
assert log.count('position updated to')>=3
key('stop',12);idle();assert not api('/api/status')['playing'];state=api('/api/youtube/state');assert state['query']=='me at the zoo' and state['page']==0 and state['index']==0,state
ret=d.remote(H,'tail -60 /var/hr54-persist/jellyfin/log/tv-events.log');assert 'source-youtube' in ret;(O/'post-reboot-youtube.return.log').write_text(ret)
for p,v in saved.items():assert api(p)==v,p
(O/'post-reboot-youtube-state.json').write_text(json.dumps(state,indent=2));print('PASS physical selection, standalone post-reboot H.264/AAC hardware playback, STOP, restored search/page/focus and other-source state isolation',flush=True)
key('exit');key('exit');key('exit');api('/api/tv/exit',{})
