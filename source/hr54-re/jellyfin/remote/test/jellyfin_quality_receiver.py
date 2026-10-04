#!/usr/bin/env python3
"""Authorized receiver quality soak. Secrets/raw upstream URLs never leave memory.
Run only on an idle receiver. Stores metadata, allowlisted FFmpeg decisions and
hardware counts; preserves login/browse/IPTV checkpoints. Leaves High selected.
"""
import argparse, importlib.util, json, pathlib, re, time, urllib.parse, urllib.request
R=pathlib.Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('deploy',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
a=argparse.ArgumentParser();a.add_argument('--receiver',default='192.168.88.103');a.add_argument('--item',required=True);a.add_argument('--rates',default='8000000,12000000,16000000');a.add_argument('--seconds',type=int,default=180);a.add_argument('--output',required=True);args=a.parse_args()
B='http://'+args.receiver+':8130';O=pathlib.Path(args.output);O.mkdir(parents=True,exist_ok=True)
def api(p,body=None):
 with urllib.request.urlopen(urllib.request.Request(B+p,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=90) as r:return json.load(r)
def remote(cmd):return d.remote(args.receiver,cmd)
assert not api('/api/status')['playing'], 'receiver must be idle'
before={p:api(p) for p in ['/api/auth/status','/api/tv/state','/api/iptv/state']}
# Read existing authentication only into process memory.
raw=remote('cat /var/hr54-persist/jellyfin/config/token')
token=next(x.strip().removeprefix('hr54-root# ') for x in raw.splitlines() if re.fullmatch('[a-zA-Z0-9_-]{30,512}',x.strip().removeprefix('hr54-root# ')))
raw=None
JF='http://192.168.88.38:8096/'
def jf(p):
 with urllib.request.urlopen(urllib.request.Request(JF+p,headers={'X-Emby-Token':token}),timeout=30) as r:return json.load(r)
def ffmpeg(item):
 name=next(x['Name'] for x in jf('System/Logs') if item in x['Name'])
 with urllib.request.urlopen(urllib.request.Request(JF+'System/Logs/Log?name='+urllib.parse.quote(name),headers={'X-Emby-Token':token}),timeout=30) as r:raw=r.read().decode(errors='replace')
 # Extract values only, never full log lines, source paths or command strings.
 out={}
 for key in ['-codec:v:0','-codec:a:0','-maxrate','-b:v','-ac','-ab']:
  vals=re.findall(r'(?:^|\s)'+re.escape(key)+r'\s+([a-zA-Z0-9_]+)',raw)
  out[key]=vals[0] if vals else None
 containers=re.findall(r'Output #\d+, ([a-zA-Z0-9_]+),',raw)
 out['container']=containers[0] if containers else None
 rates=re.findall(r'bitrate=\s*([0-9.]+)kbits/s',raw)
 out['last_mux_bitrate_kbps']=float(rates[-1]) if rates else None
 out['video_copy']=out['-codec:v:0']=='copy'
 out['audio_stereo']=out['-ac']=='2'
 out['progress_seconds']=[float(m)*60+float(s) for m,s in re.findall(r'time=\d+:(\d+):(\d+\.\d+)',raw)[-2:]]
 return out

def hardware():
 raw=remote('tail -n 1800 /var/viewer/messages.log')
 # Only counts/positions escape; receiver logs can contain opaque relay URLs.
 pos=re.findall(r'position updated to\s+([0-9.]+)',raw)
 return {'mpeg_recognized':raw.count('TransportType MPEG '),'h264_mentions':len(re.findall(r'H264|H\.264',raw)),'avc_pes_recognized':raw.count('encoding Mpeg4Pes'),'ac3_mentions':raw.count('AC3'),'decoder_config':raw.count('configured decodecontext inputchannel'),'frame_presented':raw.count('notifyFramePresented#ENTER'),'position_updates':len(pos),'last_positions':pos[-5:]}
def diagnostic():
 raw=remote('tail -n 180 /var/hr54-persist/jellyfin/log/jf.log')
 return [x[x.index('Jellyfin playback:'):] for x in raw.splitlines() if 'Jellyfin playback:' in x][-2:]
def key(k):
 with urllib.request.urlopen('http://'+args.receiver+':8080/remote/processKey?hold=keyPress&key='+k,timeout=10) as r:r.read()
item=jf('Items/'+args.item)
source=item['MediaSources'][0]
metadata={'item':args.item,'name':item['Name'],'duration':item.get('RunTimeTicks',0)/1e7,'bitrate':source.get('Bitrate'),'streams':[{k:v.get(k) for k in ['Type','Codec','Width','Height','BitRate','Channels']} for v in source.get('MediaStreams',[])]}
(O/'source.json').write_text(json.dumps(metadata,indent=2))
print('SOURCE',json.dumps(metadata),flush=True)
results=[]
try:
 for rate in [int(x) for x in args.rates.split(',')]:
  assert rate in [8000000,12000000,16000000]
  assert not api('/api/status')['playing']
  api('/api/settings',{'jellyfinVideoBitrate':rate})
  run={'rate':rate,'samples':[],'duration_requested':args.seconds}
  print('START',rate,api('/api/play',{'itemId':args.item,'returnToTv':True}),flush=True)
  started=time.monotonic()
  while time.monotonic()-started<args.seconds:
   time.sleep(min(30,args.seconds-(time.monotonic()-started)))
   sample={'wall_seconds':round(time.monotonic()-started,1),'state':api('/api/status'),'hardware':hardware()}
   run['samples'].append(sample)
   (O/('receiver-'+str(rate)+'.json')).write_text(json.dumps(run,indent=2))
   print('SAMPLE',rate,json.dumps(sample),flush=True)
   if not sample['state'].get('playing'):break
  run['ffmpeg']=ffmpeg(args.item);run['diagnostic']=diagnostic()
  # If a full-length run completed, restart at zero to exercise transport.
  if not api('/api/status')['playing']:
   run['natural_end']=run['samples'][-1]['wall_seconds']>=metadata['duration']-10
   run['early_termination']=not run['natural_end']
   api('/api/play',{'itemId':args.item,'returnToTv':True});time.sleep(15)
  run['pause']=api('/api/playback/pause',{});assert run['pause']['paused']
  time.sleep(5);run['pause_state']=api('/api/status')
  run['resume']=api('/api/playback/resume',{});assert not run['resume']['paused']
  time.sleep(15)
  run['seek']=api('/api/playback/seek',{'seconds':120});time.sleep(30)
  run['seek_state']=api('/api/status');run['seek_hardware']=hardware();run['seek_ffmpeg']=ffmpeg(args.item)
  assert run['seek_state']['playing'] and run['seek_state']['elapsed']>=130
  assert not run['seek_ffmpeg']['video_copy']
  run['seek_diagnostic']=diagnostic()
  # Physical STOP path also verifies native key watcher and TV return.
  key('stop');time.sleep(12);run['stop_state']=api('/api/status');assert not run['stop_state']['playing']
  run['tv_return_running']='Running appId' in remote("/opt/middleware_core/system/tv/uconntest '<com.ucentric.pvruconnect.DirectTest command=\"itvGetAppStatus\" sessionId=\"0\"/>' 2>&1")
  (O/('receiver-'+str(rate)+'.json')).write_text(json.dumps(run,indent=2));results.append(run)
  print('COMPLETE',rate,json.dumps({k:v for k,v in run.items() if k not in ['samples']}),flush=True)
finally:
 if api('/api/status').get('playing'):api('/api/playback/stop',{})
 api('/api/settings',{'jellyfinVideoBitrate':12000000})
 after={p:api(p) for p in before}
 assert before==after,'authentication/checkpoints changed'
 (O/'state-preservation.json').write_text(json.dumps({'unchanged':before==after,'quality':api('/api/settings')['settings']},indent=2))
print('PASS transport, physical STOP and state preservation; High restored. Check per-level natural_end/early_termination for soak stability.',flush=True)
