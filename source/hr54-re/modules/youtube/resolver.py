#!/usr/bin/env python3
"""Bounded receiver-only yt-dlp facade; backend supplies IDs/search, never URLs."""
import sys,json,re,os,pathlib,fcntl,resource
resource.setrlimit(resource.RLIMIT_AS,(128*1024*1024,128*1024*1024))
resource.setrlimit(resource.RLIMIT_CPU,(50,50))
BASE=pathlib.Path(os.environ.get('REBOX_YOUTUBE_RUNTIME','/var/hr54-persist/jellyfin/youtube'))
_lock=open(BASE/'update.lock','a');fcntl.flock(_lock,fcntl.LOCK_SH)
sys.path.insert(0,str(BASE/'yt-dlp.zip'))
from yt_dlp import YoutubeDL
from yt_dlp.utils import DownloadError
class Quiet:
 def debug(self,msg):pass
 def warning(self,msg):pass
 def error(self,msg):pass
opts={'quiet':True,'logger':Quiet(),'skip_download':True,'cachedir':False,'socket_timeout':12,'retries':0,'extractor_retries':0,'noplaylist':True,'js_runtimes':{'quickjs':{'path':str(BASE/'bin/qjs')}},'format':'bv[vcodec^=avc1][height<=1080][tbr<=8000]+ba[acodec^=mp4a.40.2]/b[vcodec^=avc1][acodec^=mp4a.40.2]','extractor_args':{'youtube':{'player_client':['visionos']}}}
def avc(f):
 rate=f.get('vbr') or f.get('tbr')
 return (f.get('vcodec') or '').startswith(('avc1','avc3','h264')) and 0<(f.get('width') or 0)<=1920 and 0<(f.get('height') or 0)<=1080 and rate is not None and 0<rate<=8000 and f.get('protocol') in ('https','http') and bool(f.get('url'))
def aac(f):return (f.get('acodec') or '').startswith(('mp4a.40.2','aac')) and f.get('protocol') in ('https','http') and bool(f.get('url'))
def resolve_once(auth=False):
 vid=sys.argv[2]
 if not re.fullmatch(r'[A-Za-z0-9_-]{11}',vid):raise ValueError('Invalid video ID')
 o=dict(opts)
 if auth:o.update(cookiefile=str(BASE/'cookies.txt'),extractor_args={'youtube':{'player_client':['web_embedded']}})
 with YoutubeDL(o) as y:d=y.extract_info('https://www.youtube.com/watch?v='+vid,download=False)
 video=sorted([f for f in d.get('formats',[]) if avc(f)],key=lambda f:(f.get('height') or 0,f.get('fps') or 0,f.get('vbr') or f.get('tbr') or 0),reverse=True)
 audio=sorted([f for f in d.get('formats',[]) if aac(f) and f.get('vcodec')=='none'],key=lambda f:f.get('abr') or f.get('tbr') or 0,reverse=True)
 if not video:raise ValueError('No compatible H.264 HTTP format')
 v=video[0];a=v if aac(v) else (audio[0] if audio else None)
 if not a:raise ValueError('No compatible AAC-LC HTTP format')
 return {'id':vid,'title':str(d.get('title') or vid)[:256],'duration':d.get('duration'),'videoUrl':v['url'],'audioUrl':a['url'],'videoFormat':v.get('format_id'),'audioFormat':a.get('format_id')}
def resolve():
 # Authenticated web clients currently require costly JS; keep proven public
 # playback on visionos. Cookie-enabled search and direct yt-dlp remain available.
 return dict(resolve_once(False),cookiesUsed=False)
def search():
 query=sys.argv[2].strip()[:64];page=min(100,max(0,int(sys.argv[3])))
 if not query:raise ValueError('Search text required')
 o=dict(opts,extract_flat=True,playliststart=page*6+1,playlistend=page*6+7,format=None)
 if (BASE/'cookies.txt').exists():o['cookiefile']=str(BASE/'cookies.txt')
 with YoutubeDL(o) as y:d=y.extract_info('ytsearch'+str(page*6+7)+':'+query,download=False)
 entries=[x for x in d.get('entries',[]) if x and re.fullmatch(r'[A-Za-z0-9_-]{11}',x.get('id') or '')]
 return {'page':page,'hasMore':len(entries)>6,'results':[{'id':x['id'],'title':str(x.get('title') or x['id'])[:180],'channel':str(x.get('uploader') or x.get('channel') or '')[:100],'duration':x.get('duration')} for x in entries[:6]]}
try:result=resolve() if sys.argv[1]=='play' else search()
except Exception as e:
 # yt-dlp errors can include signed CDN URLs or authentication data. Persist
 # only a category; the bounded error response remains available to the RPC.
 message=str(e).lower()
 category=('tls' if any(x in message for x in ('certificate','ssl','tls')) else
           'format' if isinstance(e,ValueError) else
           'authentication' if any(x in message for x in ('sign in','private','bot','login')) else
           'extraction')
 print('YouTube resolver failed stage='+category,file=sys.stderr)
 result={'error':str(e).replace('\n',' ')[:240]}
print(json.dumps(result,separators=(',',':')))
