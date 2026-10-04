#!/usr/bin/env python3
"""Diagnostic HR54 selector; signed URLs stay ephemeral and private on-box."""
import json,sys
with open(sys.argv[1]) as f:data=json.load(f)
formats=data.get('formats',[])
def avc(f):
 c=f.get('vcodec','');rate=f.get('vbr') or f.get('tbr')
 return c.startswith(('avc1','avc3','h264')) and 0<(f.get('width') or 0)<=1920 and 0<(f.get('height') or 0)<=1080 and rate is not None and 0<rate<=8000 and f.get('url') and f.get('protocol') in ('https','http')
def aac(f):return f.get('acodec','').startswith(('mp4a.40.2','aac')) and f.get('url') and f.get('protocol') in ('https','http')
video=sorted([f for f in formats if avc(f)],key=lambda f:(f.get('height') or 0,f.get('fps') or 0,f.get('vbr') or f.get('tbr') or 0),reverse=True)
audio=sorted([f for f in formats if aac(f) and f.get('vcodec')=='none'],key=lambda f:f.get('abr') or f.get('tbr') or 0,reverse=True)
chosen=None
if video:
 v=video[0]
 if aac(v):chosen={'video':v,'audio':None,'combined':True}
 elif audio:chosen={'video':v,'audio':audio[0],'combined':False}
if not chosen:
 sys.stderr.write('No compatible AVC + AAC HTTP formats\n');sys.exit(1)
# Full formats with URLs are used only by local workers; stdout summary excludes URLs.
with open(sys.argv[2],'w') as f:json.dump(chosen,f)
keys=('format_id','vcodec','acodec','width','height','fps','tbr','vbr','abr','protocol','ext')
print(json.dumps({'id':data.get('id'),'title':data.get('title'),'duration':data.get('duration'),'thumbnail':data.get('thumbnail'),'formatCount':len(formats),'combined':chosen['combined'],'video':{k:chosen['video'].get(k) for k in keys},'audio':{k:chosen['audio'].get(k) for k in keys} if chosen['audio'] else None}))
