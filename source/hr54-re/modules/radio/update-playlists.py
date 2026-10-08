#!/usr/bin/env python3
"""Import pinned upstream playlists with genre metadata and stable station IDs."""
import argparse, json, urllib.request, hashlib, re
from pathlib import Path
REPO='https://api.github.com/repos/deroverda/recommended-radio-streams'
def get(url):
    with urllib.request.urlopen(url, timeout=30) as r: return r.read()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('output',type=Path);args=ap.parse_args()
    commit=json.loads(get(REPO+'/commits/main'))['sha']
    files=json.loads(get(REPO+'/contents/playlists?ref='+commit))
    result=['#EXTM3U'];sources=[];seen=set()
    for item in sorted(files,key=lambda x:x['name']):
        if not item['name'].endswith('.m3u'):continue
        raw=get(item['download_url']);lines=raw.decode('utf-8-sig').splitlines()
        group=next((s[8:].strip() for s in lines if s.startswith('# Group:')),item['name'][:-4].replace('-',' ').title())
        name=None
        for line in lines:
            line=line.strip()
            if line.startswith('#EXTINF:'):name=line.split(',',1)[1].strip()
            elif name and line and not line.startswith('#'):
                if line.startswith(('http://','https://')) and (group,line) not in seen:
                    seen.add((group,line));result += [f'#EXTINF:-1 group-title="{group.replace(chr(34), chr(39))}",{name}',line]
                name=None
        sources.append({'name':item['name'],'sha256':hashlib.sha256(raw).hexdigest()})
    args.output.mkdir(parents=True,exist_ok=True)
    tmp=args.output/'stations.m3u.next';tmp.write_text('\n'.join(result)+'\n');tmp.replace(args.output/'stations.m3u')
    (args.output/'sources.json').write_text(json.dumps({'repository':REPO,'commit':commit,'files':sources,'stations':len(seen)},indent=2)+'\n')
    print(f'Imported {len(seen)} stations from {len(sources)} genres at {commit}')
if __name__=='__main__':main()
