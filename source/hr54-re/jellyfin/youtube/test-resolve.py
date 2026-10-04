#!/usr/bin/env python3
"""Execute anonymous yt-dlp on HR54 only. No host resolver or media proxy."""
import argparse,importlib.util,pathlib,time,re,json,shlex
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'tests/youtube-20261001'
s=importlib.util.spec_from_file_location('d',ROOT/'remote/deploy_iptv.py');d=importlib.util.module_from_spec(s);s.loader.exec_module(d)
p=argparse.ArgumentParser();p.add_argument('--receiver',required=True);p.add_argument('--video',default='jNQXAC9IVRw');p.add_argument('--client',default='default');p.add_argument('--seconds',type=int,default=90);a=p.parse_args()
assert re.fullmatch('[A-Za-z0-9_-]{11}',a.video);assert re.fullmatch('[a-z_,]+',a.client)
name='yt-resolve-'+a.video+'-'+a.client;prefix='/tmp/'+name
args=['/var/hr54-transfer/yt-measure',str(a.seconds),'/var/hr54-transfer/yt-python','/var/hr54-transfer/yt-dlp-fast.zip','--ignore-config','--no-cache-dir','--js-runtimes','quickjs:/var/hr54-transfer/yt-qjs','--verbose','--no-playlist','--skip-download','--dump-single-json','--socket-timeout','12','--retries','0','--extractor-retries','0','--extractor-args','youtube:player_client='+a.client,'https://www.youtube.com/watch?v='+a.video]
cmd='umask 077\nexport PYTHONHOME=/var/hr54-transfer/yt-python-home\nexport SSL_CERT_FILE=/var/hr54-persist/jellyfin/iptv/ca-certificates.crt\n'+shlex.join(args)+' >'+prefix+'.json 2>'+prefix+'.log </dev/null &\necho RESOLVE_MEASURE_PID=$!'
r=d.remote(a.receiver,cmd);print(r,flush=True);pid=int(re.search(r'RESOLVE_MEASURE_PID=(\d+)',r)[1]);samples=[]
for n in range((a.seconds+15)//5):
 time.sleep(5);r=d.remote(a.receiver,'if test -d /proc/'+str(pid)+'; then echo MEASURE_RUNNING; fi; tail -8 '+prefix+'.log; cat /proc/meminfo | head -8; for f in /proc/[0-9]*/status; do if grep -q "^Name:.*yt-" $f; then grep -E "^(Name|Pid|PPid|VmRSS|VmHWM):" $f; p=${f#/proc/}; p=${p%/status}; cat /proc/$p/stat; fi; done');samples.append(r)
 if 'MEASURE elapsed=' in r:break
log=d.remote(a.receiver,'cat '+prefix+'.log; wc -c '+prefix+'.json');(OUT/(name+'.log')).write_text(log);(OUT/(name+'-resources.json')).write_text(json.dumps(samples,indent=2));print(log,flush=True)
