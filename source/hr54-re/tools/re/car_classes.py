#!/usr/bin/env python3
"""Offline reader for HR54 dtv.car compact constant pools.

Recovered format: 0x80, ULEB body length, ULEB CP count, grouped CP
entries (type, count, entries). Types 20/21 reference the one-based global
string pool. This is a build-specific reader; unknown tags fail closed.
Method/field flag decoding is deliberately not guessed.
"""
import argparse
import json
import struct
import subprocess
import tempfile
import re
from pathlib import Path


class Reader:
    def __init__(self,data,pos=0):self.data=data;self.pos=pos
    def u(self):
        n=0
        for shift in range(0,35,7):
            c=self.data[self.pos];self.pos+=1;n|=(c&127)<<shift
            if c<128:return n
        raise ValueError('overlong ULEB')
    def z(self):
        end=self.data.index(0,self.pos);s=self.data[self.pos:end].decode('utf-8');self.pos=end+1;return s
    def take(self,n):
        b=self.data[self.pos:self.pos+n]
        if len(b)!=n:raise ValueError('truncated')
        self.pos+=n;return b


class Car:
    def __init__(self,path):
        self.data=Path(path).read_bytes()
        if self.data[:4]!=bytes.fromhex('ceecaccc'):raise ValueError('unexpected CAR magic')
        # Located by the AMS first global string and its preceding BE count.
        start=self.data.index(b'amsServer/AmsCPManager\0')
        count=int.from_bytes(self.data[start-4:start],'big')
        if not 1000<count<100000:raise ValueError('unsupported global pool')
        r=Reader(self.data,start);self.global_strings=[None]+[r.z() for _ in range(count)]
    def find(self,name):
        token=name.rsplit('/',1)[-1].encode()+b'\0'
        # Native CAR directory stores basename followed by its class offset.
        for m in reversed(list(re.finditer(re.escape(token),self.data))):
            try:
                offset=Reader(self.data,m.end()).u()
                if self.data[offset]!=0x80:continue
                r,cp,end=self.pool(offset);r.u();this=r.u()
                found=cp[this]['value']
                if '/' not in name or found==name:return offset
            except (ValueError,IndexError,UnicodeDecodeError):continue
        raise ValueError(f'class absent: {name}')
    def pool(self,offset):
        r=Reader(self.data,offset)
        if r.take(1)!=b'\x80':raise ValueError('class marker')
        length=r.u();end=r.pos+length;count=r.u();cp=[None]
        if end>len(self.data) or not 1<count<20000:raise ValueError('class bounds')
        while len(cp)<count:
            tag=r.u();n=r.u()
            if not 0<n<count or len(cp)+n>count:raise ValueError('CP group bounds')
            for _ in range(n):
                if tag in (20,21):v=self.global_strings[r.u()]
                elif tag in (1,7,8):v=r.z()
                elif tag in (9,10,11,12):v=[r.u(),r.u()]
                elif tag==3:v=r.u()
                elif tag in (4,):v=r.take(4).hex()
                elif tag in (5,6):v=r.take(8).hex()
                else:raise ValueError(f'unsupported CP tag {tag} at {r.pos:#x}')
                cp.append(dict(tag=tag,value=v))
                if tag in (5,6):cp.append(None) # JVM two-slot constants
        return r,cp,end
    def describe(self,name):
        off=self.find(name);r,cp,end=self.pool(off)
        def resolve(i,seen=()):
            if i<=0 or i>=len(cp):return f'#{i}'
            if i in seen:return f'cycle#{i}'
            s=cp[i]
            if s is None:return f'reserved#{i}'
            v=s['value']
            if s['tag'] in (9,10,11,12):return ' :: '.join(resolve(x,seen+(i,)) for x in v)
            return str(v)
        flags=r.u();this=r.u();superclass=r.u();interfaces=[r.u() for _ in range(r.u())]
        fields=[]
        for _ in range(r.u()):
            f,n,t=r.u(),r.u(),r.u();value=r.u() if f in (2,9,10) else None
            signature=None
            peek=Reader(self.data,r.pos);extra=peek.u()
            if 127<extra<len(cp) and cp[extra] and cp[extra]['tag'] in (1,21):
                text=str(cp[extra]['value'])
                if '<' in text and text.startswith(('L','[')):
                    signature=resolve(extra);r.pos=peek.pos
            fields.append(dict(flags=f,name=resolve(n),descriptor=resolve(t),constant=resolve(value) if value else None,signature=signature))
        methods=[]
        for _ in range(r.u()):
            at=r.pos;f,n,t=r.u(),r.u(),r.u()
            if f in (0x8f,0x9b): # observed native declaration modifier indexes
                stack=locals_=0;code_at=r.pos;code=b'';handlers=[]
            else:
                meta=r.u()
                if meta&~1:raise ValueError(f'unsupported method metadata {meta} at {at:#x}')
                stack,locals_,length=r.u(),r.u(),r.u();code_at=r.pos;code=r.take(length)
                handlers=[dict(start=r.u(),end=r.u(),handler=r.u(),type=resolve(r.u())) for __ in range(r.u())] if meta&1 else []
            throws=[];signature=None
            # Attribute flags are compact modifier indexes, not JVM bitmasks.
            # Recognize only self-describing trailers: generic descriptor CP,
            # or a short list of class CP indexes (declared exceptions).
            peek=Reader(self.data,r.pos);extra=peek.u()
            if 255<extra<len(cp) and cp[extra] and cp[extra]['tag'] in (1,21) and str(cp[extra]['value']).startswith('('):
                signature=resolve(extra);r.pos=peek.pos
            elif 0<extra<20:
                candidates=[peek.u() for __ in range(extra)]
                if all(x<len(cp) and cp[x] and cp[x]['tag'] in (7,20) for x in candidates):
                    throws=[resolve(x) for x in candidates];r.pos=peek.pos
            if r.pos>end or n>=len(cp) or t>=len(cp):raise ValueError(f'method bounds at {at:#x}')
            methods.append(dict(name=resolve(n),descriptor=resolve(t),flags=f,offset=hex(at),code_offset=hex(code_at),
                                max_stack=stack,max_locals=locals_,code=code.hex(),handlers=handlers,throws=throws,signature=signature))
        if r.pos!=end:raise ValueError(f'class end mismatch {r.pos:#x} != {end:#x}')
        return dict(name=resolve(this),offset=hex(off),end=hex(end),flags=flags,this_index=this,super_index=superclass,
                    superclass=resolve(superclass),interfaces=[resolve(i) for i in interfaces],
                    fields=fields,methods=methods,constant_pool=[dict(index=i,**s,resolved=resolve(i)) for i,s in enumerate(cp) if s])


def class_for_disassembly(d):
    """Repackage CP/code for javap only. Access flags intentionally normalized.

    Not executable output: use solely as a disassembly transport, never a JVM
    replacement for the compact class or an assertion about original access.
    """
    def h(v):return struct.pack('>H',v)
    def w(v):return struct.pack('>I',v)
    entries={s['index']:s for s in d['constant_pool']};count=max(entries)+1;encoded={}
    def utf(s):
        nonlocal count
        idx=count;count+=1
        # JVM modified UTF-8 encoding, including UTF-16 surrogate code units.
        out=bytearray()
        for off in range(0,len(s.encode('utf-16-be')),2):
            code=int.from_bytes(s.encode('utf-16-be')[off:off+2],'big')
            if 1<=code<128:out.append(code)
            elif code<2048:out.extend((0xc0|(code>>6),0x80|(code&63)))
            else:out.extend((0xe0|(code>>12),0x80|((code>>6)&63),0x80|(code&63)))
        encoded[idx]=b'\x01'+h(len(out))+out
        return idx
    for i,s in entries.items():
        t,v=s['tag'],s['value']
        if t in (1,21):
            j=utf(v);encoded[i]=encoded.pop(j);count-=1
        elif t in (7,20):encoded[i]=b'\x07'+h(utf(v))
        elif t==8:encoded[i]=b'\x08'+h(utf(v))
        elif t in (9,10,11,12):encoded[i]=bytes([t])+h(v[0])+h(v[1])
        elif t==3:encoded[i]=b'\x03'+w(v&0xffffffff)
        elif t in (4,5,6):encoded[i]=bytes([t])+bytes.fromhex(v)
        else:raise ValueError('unsupported JVM conversion')
    code_name=utf('Code');methods=[]
    for m in d['methods']:
        name=utf(m['name']);desc=utf(m['descriptor']);code=bytes.fromhex(m['code'])
        body=h(m['max_stack'])+h(m['max_locals'])+w(len(code))+code+h(0)+h(0)
        access=8 if m['name']=='<clinit>' else 1
        if not code:methods.append(h(0x101)+h(name)+h(desc)+h(0))
        else:methods.append(h(access)+h(name)+h(desc)+h(1)+h(code_name)+w(len(body))+body)
    pool=b''.join(encoded.get(i,b'') for i in range(1,count))
    return b'\xca\xfe\xba\xbe'+h(0)+h(49)+h(count)+pool+h(0x21)+h(d['this_index'])+h(d['super_index'])+h(0)+h(0)+h(len(methods))+b''.join(methods)+h(0)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('car');p.add_argument('classes',nargs='+')
    p.add_argument('--javap',action='store_true')
    a=p.parse_args();c=Car(a.car);ds=[c.describe(n) for n in a.classes]
    if a.javap:
        for d in ds:
            print(f"// {d['name']} CAR {d['offset']} (normalized access; original method file offsets in JSON)",flush=True)
            with tempfile.TemporaryDirectory(prefix='hr54-car-') as tmp:
                f=Path(tmp)/'Recovered.class';f.write_bytes(class_for_disassembly(d))
                subprocess.run(['javap','-p','-c',str(f)],check=True)
    else:print(json.dumps(ds,indent=2))
