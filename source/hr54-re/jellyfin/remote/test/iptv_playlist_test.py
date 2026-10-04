#!/usr/bin/env python3
"""Compare every native-parsed actual entry against independent Python parsing."""
import json,pathlib,re,subprocess
r=pathlib.Path(__file__).resolve().parents[1];playlist=r.parent/'iptv/eng.m3u'
subprocess.run(['cc','-std=c11','-O1','-o','/tmp/iptv-unit',str(r/'test/iptv_unit.c')],check=True)
subprocess.run(['/tmp/iptv-unit',str(playlist)],check=True)
native=json.loads(subprocess.check_output(['/tmp/iptv-unit',str(playlist),'--dump']))
expected=[];entry=None;vlc=ua=ref=0
for line in playlist.read_text().splitlines():
 if line.startswith('#EXTINF:'):
  delimiter=re.search(r',(?=(?:[^"]*"[^"]*")*[^"]*$)',line)
  assert delimiter,line
  entry={k:v for k,v in re.findall(r'([\w-]+)="([^"]*)"',line[:delimiter.start()])}
  entry['name']=line[delimiter.end():].strip()
 elif line.startswith('#EXTVLCOPT:'):
  key,value=line[len('#EXTVLCOPT:'):].split('=',1);entry[key]=value;vlc+=1;ua+=key=='http-user-agent';ref+=key=='http-referrer'
 elif line and not line.startswith('#'):
  assert entry;entry['url']=line.strip();expected.append(entry);entry=None
assert len(expected)==2096 and vlc==93 and ua==63 and ref==30
for c,e in zip(native,expected):
 for k in ('name','tvg-id','tvg-logo','group-title','http-user-agent','http-referrer','url'):assert c[k]==e.get(k,''),(k,c['name'],c[k],e.get(k))
assert len(set(c['id'] for c in native))==2096
print('PASS every real entry: name, tvg-id, logo, group, URL/query, 63 UA + 30 referrer EXTVLCOPT associations; unique stable IDs')
