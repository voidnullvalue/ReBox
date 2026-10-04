#!/usr/bin/env python3
"""Recover MediaPlayer schemas and DirectTest branches without running firmware.

Only canonical generated constructors/Add methods are decoded. The command
inventory recognizes command-local String.equals branches in host javap output;
it records bytecode offsets, attributes and calls rather than guessing behavior.
"""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path
from car_classes import Car, class_for_disassembly


def first_constant(m,cp):
    b=bytes.fromhex(m['code'])
    if len(b)>2 and b[0]==0x2a:
        i=b[2] if b[1]==0x12 else int.from_bytes(b[2:4],'big') if b[1]==0x13 else 0
        s=cp.get(i,{})
        if s.get('tag')==3:return s['value']&0xffffffff


def schemas(car):
    proxy=car.describe('com/directv/dvrcore/impl/MediaPlayer/MediaPlayerProxy')
    names=sorted({s['value'] for s in proxy['constant_pool'] if s['tag'] in (7,20)
                  and '/MediaPlayerProxyIPC$' in str(s['value'])})
    result=[];failures=[]
    for name in names:
        try:d=car.describe(name)
        except (ValueError,IndexError) as e:failures.append(dict(class_name=name,error=str(e)));continue
        cp={s['index']:s for s in d['constant_pool']};fields=[];tag=None
        for m in d['methods']:
            value=first_constant(m,cp)
            if m['name']=='<init>' and m['descriptor']=='()V':tag=value
            elif m['name'].startswith('Add'):
                fields.append(dict(name=m['name'][3:],tag=hex(value) if value is not None else None,
                                   descriptor=m['descriptor'],offset=m['offset']))
        result.append(dict(name=name.rsplit('$',1)[-1],class_name=name,offset=d['offset'],
                           message_tag=hex(tag) if tag is not None else None,fields=fields))
    return dict(schemas=result,failures=failures)


def javap(d):
    with tempfile.TemporaryDirectory(prefix='hr54-car-inventory-') as tmp:
        p=Path(tmp)/'Recovered.class';p.write_bytes(class_for_disassembly(d))
        return subprocess.check_output(['javap','-p','-c',str(p)],text=True)


def commands(car):
    d=car.describe('com/ucentric/pvruconnect/DirectTest');text=javap(d)
    result=[]
    for method in ('doRequest1','doRequest2'):
        match=re.search(r'^  public .*?\b'+method+r'\(.*?\n(.*?)(?=^  (?:public|static)|^})',text,re.M|re.S)
        if not match:continue
        lines=match[1].splitlines();branches=[]
        for i,line in enumerate(lines):
            if 'aload_1' not in line:continue
            if i+3>=len(lines):continue
            string=re.search(r'(\d+):\s+ldc(?:_w)?\s+.*?// (?:Utf8|String) (.+)',lines[i+1])
            if string and 'java/lang/String.equals:' in lines[i+2] and 'ifeq' in lines[i+3]:
                branches.append((i,int(string[1]),string[2]))
        for k,(i,offset,name) in enumerate(branches):
            block=lines[i:branches[k+1][0] if k+1<len(branches) else len(lines)]
            props=[]
            for j,line in enumerate(block[:-1]):
                s=re.search(r'// (?:Utf8|String) (.+)',line)
                if s and 'Properties.getProperty:' in block[j+1]:
                    # Two-argument getProperty has a default string after its key.
                    if '(Ljava/lang/String;Ljava/lang/String;)' in block[j+1] and j:
                        key=re.search(r'// (?:Utf8|String) (.+)',block[j-1])
                        if key:props.append(key[1])
                    else:props.append(s[1])
            calls=sorted(set(m[1] for line in block if (m:=re.search(r'// (?:InterfaceMethod|Method) (.+)',line))
                             and not m[1].startswith(('java/','"<init>"','com/ucentric/log/'))))
            result.append(dict(command=name,method=method,method_file_offset=next(m['code_offset'] for m in d['methods'] if m['name']==method),
                               bytecode_offset=offset,attributes=sorted(set(props)),calls=calls))
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('car');p.add_argument('mode',choices=['schemas','commands'])
    a=p.parse_args();c=Car(a.car);print(json.dumps(schemas(c) if a.mode=='schemas' else commands(c),indent=2))
