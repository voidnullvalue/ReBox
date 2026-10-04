#!/usr/bin/env python3
"""Additional supplied HTTP HLS and metadata-Referer HTTPS cases."""
import importlib.util,json,pathlib,time,urllib.request,uuid
R=pathlib.Path(__file__).resolve().parents[1];O=R.parent/'tests/iptv-20261001';H='192.168.88.103';B='http://'+H+':8130'
sp=importlib.util.spec_from_file_location('d',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
def api(p,body=None):
 try:return json.load(urllib.request.urlopen(urllib.request.Request(B+p,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=90))
 except urllib.error.HTTPError as e:return json.load(e)
for name,query in [('Arirang TV (1080p)','Arirang'),('GTV (540p)','GTV')]:
 assert not api('/api/status')['playing'];c=next(c for c in api('/api/iptv/channels?limit=60&query='+query)['channels'] if c['name']==name)
 marker='IPTV_CASE_'+uuid.uuid4().hex;d.remote(H,'echo '+marker+' >> /var/viewer/messages.log')
 result=api('/api/iptv/play',{'channelId':c['id']});print(name,result,flush=True)
 if not result.get('playing'):continue
 try:
  time.sleep(15);status=api('/api/status');assert status.get('source')=='iptv' and status['playing'];raw=d.remote(H,'tail -2200 /var/viewer/messages.log');at=raw.find(marker+'\n');assert at>=0;log=raw[at:].split('hr54-root#')[0]
  assert '/iptv-stream/' in log and 'TransportType MPEG ' in log and 'configured decodecontext inputchannel' in log and 'notifyFramePresented#ENTER' in log
  tag='real-http' if query=='Arirang' else 'real-referer';(O/(tag+'.receiver.log')).write_text(log);(O/(tag+'.native.log')).write_text(d.remote(H,'tail -65 /var/hr54-persist/jellyfin/log/jf.log'));(O/(tag+'.status.json')).write_text(json.dumps(status,indent=2));print('PASS',name,'native relay, MPEG TS recognition, decoder/frame, continuing playback',flush=True)
 finally:api('/api/iptv/stop',{});time.sleep(12)
# Actual unsupported playlist entries remain indexed and fail without a session.
import subprocess
entries=json.loads(subprocess.check_output(['/tmp/iptv-unit',str(R.parent/'iptv/eng.m3u'),'--dump']))
for protocol,entry in [('SRT',next(c for c in entries if c['url'].startswith('srt:'))),('DASH',next(c for c in entries if '.mpd' in c['url']))]:
 error=api('/api/iptv/play',{'channelId':entry['id']});assert protocol in error['error'];assert not api('/api/status')['playing'];assert not api('/api/iptv/status')['active'];print('PASS actual',protocol,'entry:',error,flush=True)
