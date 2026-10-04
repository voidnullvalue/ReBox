#!/usr/bin/env python3
"""Verify completed, sanitized receiver quality evidence; no network access."""
import json,pathlib,sys
root=pathlib.Path(sys.argv[1]);source=json.loads((root/'source.json').read_text())
video=next(s for s in source['streams'] if s['Type']=='Video')
assert video['Codec']=='h264' and video['Height']==1080
rows=[]
for rate in [8000000,12000000,16000000]:
 r=json.loads((root/('receiver-'+str(rate)+'.json')).read_text())
 assert r['rate']==rate and any('requested ceiling='+str(rate)+';' in x for x in r['diagnostic'])
 f=r['ffmpeg'];assert f['container']=='mpegts' and f['-codec:a:0']=='ac3' and f['-ac']=='2'
 assert f['video_copy']==(video['BitRate']<rate-192000)
 assert any(s['hardware']['mpeg_recognized'] for s in r['samples'])
 assert any(s['hardware']['decoder_config'] for s in r['samples'])
 assert any(s['hardware']['frame_presented'] for s in r['samples'])
 progressing=[s for s in r['samples'] if s['state']['playing']]
 assert len(progressing)>=5
 positions=[float(s['hardware']['last_positions'][-1]) for s in progressing]
 assert all(b>a for a,b in zip(positions,positions[1:]))
 assert r['pause']['paused'] and r['pause_state']['paused'] and not r['resume']['paused']
 assert r['seek']['startSeconds']==120 and r['seek_state']['playing'] and r['seek_state']['elapsed']>=130
 assert not r['seek_ffmpeg']['video_copy']
 assert not r['stop_state']['playing'] and r['tv_return_running']
 if rate!=8000000:
  if rate==12000000:
   assert r['natural_end'] and f['progress_seconds'][-1]>=source['duration']-5
  else:
   assert r.get('natural_end') or r.get('early_termination')
  assert any(s['hardware'].get('avc_pes_recognized') for s in r['samples'])
 rows.append({'rate':rate,'method':'video copy + AC3 conversion' if f['video_copy'] else 'H264 encode + AC3 conversion','video_maxrate':f['-maxrate'],'observed_mux_kbps':f.get('last_mux_bitrate_kbps'),'soak_seconds':r['samples'][-1]['wall_seconds'],'full_title':bool(r.get('natural_end')),'early_termination':bool(r.get('early_termination')),'transport_and_tv_return':True})
assert json.loads((root/'state-preservation.json').read_text())['unchanged']
print(json.dumps({'source':source['name'],'source_video_bitrate':video['BitRate'],'results':rows,'default':12000000},indent=2))
