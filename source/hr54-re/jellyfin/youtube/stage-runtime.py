#!/usr/bin/env python3
"""Stage additive private runtime with receiver identity and hash checks."""
import argparse,importlib.util,pathlib,json,urllib.request,hashlib,time,re
ROOT=pathlib.Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('d',ROOT/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
a=argparse.ArgumentParser();a.add_argument('--receiver',required=True);a.add_argument('--cookies-file',type=pathlib.Path);args=a.parse_args();h=args.receiver
status=json.load(urllib.request.urlopen('http://'+h+':8130/api/status',timeout=10));assert status['receiverHost']==h
base='/var/hr54-persist/jellyfin/youtube/';build=pathlib.Path('/tmp/hr54-youtube-build');stamp='yt-runtime-'+str(int(time.time()))
files=[(build/'yt-python','bin/python','700','yt-python'),(build/'quickjs-2026-06-04/qjs','bin/qjs','700','yt-qjs'),(build/'hr54-remux-small','bin/remux','700','yt-remux-v2'),(build/'relay','bin/relay','700',None),(build/'exec-guard','bin/exec-guard','700',None),(build/'resolver.pyc','resolver.pyc','600',None),(build/'updater.pyc','updater.pyc','600',None),(build/'python314-fast.zip','python/lib/python314.zip','600','yt-python314-fast.zip'),(build/'sysconfig.pyc','python/lib/python3.14/_sysconfigdata__linux_.pyc','600','yt-sysconfig.pyc'),(build/'yt-dlp-fast.zip','yt-dlp.zip','600','yt-dlp-fast.zip'),(build/'yt-dlp-wrapper','bin/yt-dlp','700',None),(build/'yt-dlp.conf','yt-dlp.conf','600',None)]
cookie=args.cookies_file or pathlib.Path('/tmp/hr54-youtube-private/cookies.txt')
if cookie.exists():files.append((cookie,'cookies.txt','600',None))
status=json.load(urllib.request.urlopen('http://'+h+':8130/api/status',timeout=10));assert not status['playing']
try:assert not json.load(urllib.request.urlopen('http://'+h+':8130/api/youtube/status',timeout=10))['active']
except urllib.error.HTTPError as e:
 if e.code!=404:raise
commands=['set -e',f'mkdir -p {base}bin {base}python/lib/python3.14/lib-dynload',f'chmod 700 {base} {base}bin {base}python']
current=d.remote(h,'md5sum '+' '.join(base+dest for _,dest,_,_ in files))
changed=[]
commands += [f'mkdir -p {base}backup/{stamp}']
for i,(src,dest,mode,already) in enumerate(files):
 digest=hashlib.md5(src.read_bytes()).hexdigest()
 if digest+'  '+base+dest in current:continue
 name=stamp+'-'+str(i);d.tftp.put(h,1069,str(src),name)
 changed.append(dest);transfer='/var/hr54-transfer/'+name
 commands += [f'if [ -f {base+dest} ]; then cp {base+dest} {base}backup/{stamp}/'+pathlib.Path(dest).name+'; fi']
 commands += [f'test "$(md5sum {transfer} | cut -d " " -f 1)" = {digest}',f'cp {transfer} {base+dest}.next',f'chmod {mode} {base+dest}.next',f'test "$(md5sum {base+dest}.next | cut -d " " -f 1)" = {digest}']
 if dest=='cookies.txt':commands += [f'chmod 600 {transfer}']
commands += ['sync']+[f'mv {base+dest}.next {base+dest}' for dest in changed]+[f'printf "2026.08.19\\n" > {base}version',f'touch {base}update-status.json', 'sync']
r=d.remote(h,'\n'.join(commands));assert 'Error' not in r and 'failed' not in r.lower();print('Runtime staged and hash verified; '+('private cookie file mode 0600' if cookie.exists() else 'existing cookie configuration preserved'))
