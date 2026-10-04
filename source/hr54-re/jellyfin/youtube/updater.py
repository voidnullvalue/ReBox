#!/usr/bin/env python3
"""Idle-only receiver updater: official HTTPS, checksum, pyc, smoke, atomic swap."""
import sys,os,time,json,pathlib,subprocess,hashlib,zipfile,py_compile,tempfile,urllib.request,fcntl,resource,shutil
BASE=pathlib.Path('/var/hr54-persist/jellyfin/youtube');STAMP=BASE/'update-status.json'
resource.setrlimit(resource.RLIMIT_AS,(160*1024*1024,160*1024*1024))
resource.setrlimit(resource.RLIMIT_CPU,(300,300));os.nice(15);os.umask(0o077)
def state():
 for path,key in [('/api/status','playing'),('/api/iptv/status','active'),('/api/youtube/status','active')]:
  with urllib.request.urlopen('http://127.0.0.1:8130'+path,timeout=3) as r:
   if json.load(r).get(key):return False
 return True
def fetch(url,cap):
 if not url.startswith('https://'):raise ValueError('HTTPS required')
 cmd=[str(BASE/'bin/exec-guard'),'/var/hr54-persist/jellyfin/bin/hr54-iptv-fetch',url,'/var/hr54-persist/jellyfin/iptv/ca-certificates.crt','HR54-YouTube-Updater/1','','30']
 with subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL) as p:
  for i in range(3):
   if len(p.stdout.readline(8194))>8193:raise ValueError('Invalid fetch header')
  b=p.stdout.read(cap+1)
  if len(b)>cap:p.kill();raise ValueError('Release exceeds bounds')
  if p.wait(timeout=5):raise ValueError('HTTPS release fetch failed')
  return b
def atomic_json(data):
 tmp=STAMP.with_suffix('.next');tmp.write_text(json.dumps(data));os.chmod(tmp,0o600);os.replace(tmp,STAMP)
try:
 updater_lock=open(BASE/'updater.lock','a');fcntl.flock(updater_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 if not state():sys.exit(0)
 current=(BASE/'version').read_text().strip()
 # Timestamp the attempt before heavy work, so a killed worker cannot retry
 # every minute. The separate lock also permits safe stale-temp cleanup.
 atomic_json({'checked':time.time(),'version':current,'result':'checking'})
 for stale in BASE.glob('update-*'):
  if stale.is_dir():shutil.rmtree(stale)
 release=json.loads(fetch('https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest',256*1024));tag=release['tag_name']
 import re
 if not re.fullmatch(r'20\d{2}\.\d{2}\.\d{2}',tag):raise ValueError('Unexpected release tag')
 if tag==current:atomic_json({'checked':time.time(),'version':current,'result':'current'});sys.exit(0)
 assets={x['name']:x['browser_download_url'] for x in release['assets']}
 checksum=fetch(assets['SHA2-256SUMS'],256*1024).decode();expected=None
 for line in checksum.splitlines():
  fields=line.split()
  if len(fields)==2 and fields[1].lstrip('*')=='yt-dlp':expected=fields[0]
 if not expected or not re.fullmatch('[a-fA-F0-9]{64}',expected):raise ValueError('Missing official checksum')
 data=fetch(assets['yt-dlp'],16*1024*1024)
 if hashlib.sha256(data).hexdigest()!=expected.lower():raise ValueError('Release checksum mismatch')
 if not state():sys.exit(0)
 with tempfile.TemporaryDirectory(prefix='update-',dir=BASE) as tmp:
  tmp=pathlib.Path(tmp);archive=tmp/'source.zip';archive.write_bytes(data);nextzip=tmp/'next.zip';total=0
  with zipfile.ZipFile(archive) as src,zipfile.ZipFile(nextzip,'w',zipfile.ZIP_DEFLATED) as dst:
   for item in src.infolist():
    name=pathlib.PurePosixPath(item.filename)
    if name.is_absolute() or '..' in name.parts or item.file_size>8*1024*1024:raise ValueError('Invalid archive member')
    total+=item.file_size
    if total>64*1024*1024:raise ValueError('Expanded release exceeds bounds')
    if item.is_dir():continue
    if not state():sys.exit(0)
    content=src.read(item)
    if name.suffix=='.py':
     text=tmp/'module.py';code=tmp/'module.pyc';text.write_bytes(content)
     py_compile.compile(str(text),cfile=str(code),dfile=str(name),doraise=True,invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
     dst.writestr(str(name.with_suffix('.pyc')),code.read_bytes())
    else:dst.writestr(str(name),content)
  result=subprocess.check_output([sys.executable,str(nextzip),'--ignore-config','--version'],timeout=30).decode().strip()
  if result!=tag:raise ValueError('Staged runtime version test failed')
  # Readers keep a shared lock throughout extraction; committing never changes
  # an archive beneath a zipimport reader. No waits for active playback.
  with open(BASE/'update.lock','a') as lock:
   fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
   if not state():sys.exit(0)
   shutil.copy2(BASE/'yt-dlp.zip',BASE/'yt-dlp.previous.zip')
   shutil.copy2(BASE/'version',BASE/'version.previous')
   os.chmod(nextzip,0o600);os.sync();os.replace(nextzip,BASE/'yt-dlp.zip')
   (BASE/'version.next').write_text(tag+'\n');os.replace(BASE/'version.next',BASE/'version');os.sync()
  atomic_json({'checked':time.time(),'version':tag,'previous':current,'result':'updated'})
except Exception as e:
 atomic_json({'checked':time.time(),'result':'failed','error':str(e)[:160]})
