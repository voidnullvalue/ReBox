#!/usr/bin/env python3
"""Hash-verified idle-only atomic update of small private runtime helpers."""
import pathlib,importlib.util,json,urllib.request,hashlib,time,argparse,re
R=pathlib.Path(__file__).resolve().parents[1];sp=importlib.util.spec_from_file_location('d',R/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
p=argparse.ArgumentParser();p.add_argument('--receiver',required=True);a=p.parse_args();h=a.receiver;B='http://'+h+':8130';base='/var/hr54-persist/jellyfin/';root=base+'youtube/';build=pathlib.Path('/tmp/hr54-youtube-build')
def idle():
 s=json.load(urllib.request.urlopen(B+'/api/status'));assert s['receiverHost']==h and not s['playing']
 for route in ('iptv','youtube'):assert not json.load(urllib.request.urlopen(B+'/api/'+route+'/status'))['active']
idle();expected=json.loads((R/'remote/iptv-production-hashes.json').read_text());raw=d.remote(h,'md5sum '+' '.join(base+x for x in expected));assert all(v+'  '+base+k in raw for k,v in expected.items())
files=[('relay','bin/relay','700'),('resolver.pyc','resolver.pyc','600'),('updater.pyc','updater.pyc','600'),('yt-dlp-wrapper','bin/yt-dlp','700'),('yt-dlp.conf','yt-dlp.conf','600')];stamp='yt-helpers-'+str(int(time.time()));cmd=['set -e',f'mkdir -p {root}backup/{stamp}'];receipt={}
for i,(src,dest,mode) in enumerate(files):
 name=stamp+'-'+str(i);f=build/src;digest=hashlib.md5(f.read_bytes()).hexdigest();d.tftp.put(h,1069,str(f),name)
 transfer='/var/hr54-transfer/'+name;target=root+dest;old=d.remote(h,'md5sum '+target);oldhash=re.search(r'([0-9a-f]{32})  '+re.escape(target),old)[1];receipt[dest]={'before':oldhash,'after':digest}
 cmd += [f'test "$(md5sum {target} | cut -d " " -f 1)" = {oldhash}',f'cp {target} {root}backup/{stamp}/{src}',f'test "$(md5sum {transfer} | cut -d " " -f 1)" = {digest}',f'cp {transfer} {target}.next',f'chmod {mode} {target}.next',f'test "$(md5sum {target}.next | cut -d " " -f 1)" = {digest}']
idle();cmd += ['sync']+[f'mv {root+dest}.next {root+dest}' for _,dest,_ in files]+['sync','echo YOUTUBE_HELPERS_OK']
assert 'YOUTUBE_HELPERS_OK' in d.remote(h,'\n'.join(cmd));print(json.dumps({'backup':root+'backup/'+stamp,'hashes':receipt},indent=2))
