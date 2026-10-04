#!/usr/bin/env python3
"""Exercise quality selection through stock remote key delivery and old WebKit.
Requires idle receiver; leaves High selected and returns to stock TV.
"""
import importlib.util,json,pathlib,time,urllib.request
R=pathlib.Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('deploy',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
HOST='192.168.88.103';B='http://'+HOST+':8130'
def api(p,body=None):
 with urllib.request.urlopen(urllib.request.Request(B+p,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=30) as r:return json.load(r)
def key(k,delay=.5):
 with urllib.request.urlopen('http://'+HOST+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10) as r:r.read()
 time.sleep(delay)
assert not api('/api/status')['playing']
before={p:api(p) for p in ['/api/auth/status','/api/tv/state','/api/iptv/state']}
assert api('/api/settings')['settings']['jellyfinVideoBitrate']==12000000
try:
 api('/api/tv/exit',{});key('menu',24)
 for _ in range(4):key('down')
 key('select',2)
 key('down');key('select',1)
 assert api('/api/settings')['settings']['jellyfinVideoBitrate']==16000000
 print('PASS real remote selected Maximum 16 Mbps',flush=True)
 key('up');key('up');key('select',1)
 assert api('/api/settings')['settings']['jellyfinVideoBitrate']==8000000
 print('PASS real remote selected Conservative 8 Mbps',flush=True)
 key('exit');key('select',2) # MEDIA retains Settings focus; reload saved value.
 key('down');key('select',1)
 assert api('/api/settings')['settings']['jellyfinVideoBitrate']==12000000
 print('PASS saved Conservative reloaded, then High 12 Mbps selected',flush=True)
 key('exit');key('exit')
 after={p:api(p) for p in before};assert before==after
 print('PASS real WebKit remote settings, BACK, authentication/Jellyfin/IPTV checkpoints unchanged; High restored',flush=True)
finally:
 if api('/api/settings')['settings']['jellyfinVideoBitrate']!=12000000:api('/api/settings',{'jellyfinVideoBitrate':12000000})
