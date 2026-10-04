#!/usr/bin/env python3
"""Real upstream copy/conversion checks using requests emitted by the native C
builder. Does not change receiver playback. Saves no secrets or upstream URLs.
"""
import importlib.util,json,pathlib,re,subprocess,tempfile,time,urllib.parse,urllib.request
R=pathlib.Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('deploy',R/'deploy_media_hub.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
raw=d.remote('192.168.88.103','cat /var/hr54-persist/jellyfin/config/token')
token=next(x.strip().removeprefix('hr54-root# ') for x in raw.splitlines() if re.fullmatch('[a-zA-Z0-9_-]{30,512}',x.strip().removeprefix('hr54-root# ')));raw=None
base='http://192.168.88.38:8096'
def get(p,body=None,as_json=True):
 req=urllib.request.Request(base+'/'+p.lstrip('/'),data=None if body is None else json.dumps(body).encode(),headers={'X-Emby-Token':token,'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=60) as r:return json.load(r) if as_json else r.read().decode(errors='replace')
user=get('/Users/Me')['Id']
results=[]
with tempfile.TemporaryDirectory(prefix='hr54-quality-upstream-') as td:
 t=pathlib.Path(td);binary=t/'requests'
 subprocess.run(['cc','-std=c11','-O1','-o',str(binary),str(R/'test/jellyfin_quality_unit.c')],check=True)
 for item in ['0a456b126877c597b6f762bf8ec9030f','5308c6d8c63e00cd607ca4b4d4ca577f','cdedd99fbd80ab0e7eded4a1a00c1abb']:
  source=get('Items/'+item);ms=source['MediaSources'][0]
  video=next(x for x in ms['MediaStreams'] if x['Type']=='Video')
  for rate in [12000000,16000000]:
   body=json.loads(subprocess.check_output([str(binary),'--request',user,item,str(rate)]))
   assert body['MaxStreamingBitrate']==body['DeviceProfile']['MaxStreamingBitrate']==rate
   pi=get('Items/'+item+'/PlaybackInfo',body);selected=pi['MediaSources'][0]
   assert selected['TranscodingContainer']=='ts' and selected['TranscodingSubProtocol']=='http'
   url=base+selected['TranscodingUrl'];q=urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
   assert q['VideoCodec']==['h264'] and q['AudioCodec']==['ac3']
   out={'item':item,'name':source['Name'],'rate':rate,'source_video':{k:video.get(k) for k in ['Codec','Width','Height','BitRate']},'decision':{k:v for k,v in q.items() if k in ['VideoCodec','AudioCodec','VideoBitrate','AudioBitrate','TranscodeReasons','SubtitleMethod','allowVideoStreamCopy','allowAudioStreamCopy']}}
   # Start exactly the returned URL, preserving Jellyfin's own semantics.
   # Any error is reported without its sensitive request URL.
   try:
    with urllib.request.urlopen(url,timeout=60) as r:sample=r.read(2*1024*1024)
   except Exception as e:raise RuntimeError('upstream stream failed: '+type(e).__name__) from None
   samplepath=t/'sample.ts';samplepath.write_bytes(sample)
   probe=subprocess.run(['ffprobe','-v','error','-show_entries','stream=codec_name,width,height,channels,sample_rate:format=format_name','-of','json',str(samplepath)],capture_output=True,timeout=30)
   assert probe.returncode==0
   out['probe']=json.loads(probe.stdout)
   time.sleep(3)
   name=next(x['Name'] for x in get('System/Logs') if item in x['Name'])
   log=get('System/Logs/Log?name='+urllib.parse.quote(name),as_json=False)
   out['ffmpeg']={}
   for flag in ['-codec:v:0','-codec:a:0','-maxrate','-ac','-ab']:
    vals=re.findall(r'(?:^|\s)'+re.escape(flag)+r'\s+([a-zA-Z0-9_]+)',log)
    out['ffmpeg'][flag]=vals[0] if vals else None
   if video['Codec']=='h264':assert out['ffmpeg']['-codec:v:0']=='copy',out
   else:assert out['ffmpeg']['-codec:v:0']=='libx264',out
   assert out['ffmpeg']['-codec:a:0']=='ac3' and out['ffmpeg']['-ac']=='2'
   assert out['probe']['format']['format_name']=='mpegts'
   assert any(x['codec_name']=='h264' for x in out['probe']['streams'])
   assert any(x['codec_name']=='ac3' and x['channels']==2 for x in out['probe']['streams'])
   results.append(out);print(json.dumps(out),flush=True)
print('PASS actual copy of compatible 6.1/11 Mbps H264 at 12/16 Mbps, HEVC conversion, MPEG-TS/H264/AC3 stereo; native request builder',flush=True)
