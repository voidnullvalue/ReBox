#!/usr/bin/env python3
"""Package a prepared module directory deterministically; never execute it."""
import argparse,gzip,hashlib,json,os,pathlib,re,tarfile,tempfile

def package(source,output):
    source=pathlib.Path(source).resolve();output=pathlib.Path(output).resolve()
    raw=(source/'module.json').read_bytes()
    if len(raw)>16384:raise ValueError('manifest exceeds 16 KiB')
    def unique(pairs):
        d={}
        for k,v in pairs:
            if k in d:raise ValueError('duplicate manifest key')
            d[k]=v
        return d
    manifest=json.loads(raw,object_pairs_hook=unique)
    if not re.fullmatch(r'[a-z0-9][a-z0-9._-]{0,63}',manifest['id']):raise ValueError('invalid module ID')
    for key in ('schema','moduleApi','minReboxApi'):
        if type(manifest[key]) is not int or manifest[key]!=1:raise ValueError('unsupported '+key)
    entry=manifest['entrypoint']
    def safe(name):return name and len(name.encode())<=255 and not name.startswith('/') and all(p not in ('','..','.') for p in name.split('/')) and '\\' not in name
    if not safe(entry) or not (source/entry).is_file() or not os.access(source/entry,os.X_OK):raise ValueError('invalid executable')
    paths=sorted(source.rglob('*'));total=0
    if len(paths)>4096:raise ValueError('too many entries')
    for p in paths:
        if p.is_symlink() or not (p.is_file() or p.is_dir()) or not safe(p.relative_to(source).as_posix()):raise ValueError('unsafe archive member')
        if p.is_file():total+=p.stat().st_size
    if total>128*1024*1024:raise ValueError('unpacked package exceeds limit')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent,delete=False) as tmp:
        temp=pathlib.Path(tmp.name)
        try:
            with gzip.GzipFile(filename='',mode='wb',fileobj=tmp,mtime=0) as gz,tarfile.open(fileobj=gz,mode='w',format=tarfile.USTAR_FORMAT) as tar:
                for p in paths:
                    name=p.relative_to(source).as_posix();info=tarfile.TarInfo(name);info.uid=info.gid=info.mtime=0;info.uname=info.gname=''
                    info.mode=0o700 if p.is_dir() or p.stat().st_mode&0o111 else 0o600
                    if p.is_dir():info.type=tarfile.DIRTYPE;tar.addfile(info)
                    else:
                        info.size=p.stat().st_size
                        with p.open('rb') as f:tar.addfile(info,f)
            tmp.flush();os.fsync(tmp.fileno())
            if temp.stat().st_size>64*1024*1024:raise ValueError('compressed package exceeds limit')
            temp.replace(output)
        finally:temp.unlink(missing_ok=True)
    return hashlib.sha256(output.read_bytes()).hexdigest()
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('module_dir');p.add_argument('output');args=p.parse_args();print(package(args.module_dir,args.output))
