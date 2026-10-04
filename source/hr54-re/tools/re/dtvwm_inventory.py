#!/usr/bin/env python3
"""Recover DTVWM request constructors from stack stores around the cookie.

Only constants established by simple MIPS instructions are reported. Dynamic
lengths remain null. The output is evidence, not a claim of server support.
"""
import argparse
import json
import re
from mips_elf import Elf


def stores(e, start, end):
    regs = {'$zero': 0}
    out = []
    clobber = 0
    for ins in e.instructions(start, end):
        if clobber == 1:
            for r in ('$v0','$v1','$a0','$a1','$a2','$a3','$t9'): regs.pop(r,None)
        clobber = max(0,clobber-1)
        op = [p.strip() for p in ins.op_str.split(',')]
        if ins.mnemonic == 'lui': regs[op[0]] = int(op[1], 0)<<16
        elif ins.mnemonic in ('addiu', 'ori') and len(op) == 3:
            v = regs.get(op[1])
            regs[op[0]] = None if v is None else ((v+int(op[2],0)) if ins.mnemonic == 'addiu' else v|int(op[2],0))&0xffffffff
        elif ins.mnemonic == 'move': regs[op[0]] = regs.get(op[1])
        elif ins.mnemonic == 'sw':
            m = re.fullmatch(r'(-?0x[0-9a-f]+|-?\d+)?\(\$sp\)', op[1])
            if m: out.append((ins.address, int(m[1] or '0',0), regs.get(op[0])))
        elif ins.mnemonic in ('jal','jalr'):
            clobber = 2
        elif ins.mnemonic.startswith('b') or ins.mnemonic in ('nop','jr','sb','sh'):
            pass
        elif op and op[0].startswith('$'):
            regs.pop(op[0], None)
    return out


def templates(e,start,end):
    regs={'$zero':0}
    for ins in e.instructions(start,end):
        op=[p.strip() for p in ins.op_str.split(',')]
        if ins.mnemonic=='lw' and len(op)==2:
            m=re.fullmatch(r'(-?0x[0-9a-f]+|-?\d+)\(\$gp\)',op[1])
            if m:regs[op[0]]=e.got.get(int(m[1],0),(None,''))[0]
            else:regs.pop(op[0],None)
        elif ins.mnemonic=='lui':regs[op[0]]=int(op[1],0)<<16
        elif ins.mnemonic in ('addiu','ori') and len(op)==3:
            v=regs.get(op[1]);regs[op[0]]=None if v is None else ((v+int(op[2],0)) if ins.mnemonic=='addiu' else v|int(op[2],0))&0xffffffff
        elif ins.mnemonic=='jalr':
            target=e.byaddr.get(regs.get('$t9'),{}).get('demangled','')
            if target=='memcpy' and regs.get('$a1') is not None:
                import struct
                try:
                    vals=struct.unpack('>4I',e.read(regs['$a1'],16))
                    if vals[1]==0xd123567a and vals[0]<112:yield ins.address,vals
                except ValueError:pass
            regs.clear();regs['$zero']=0
        elif ins.mnemonic not in ('sw','sb','sh','nop','jr') and not ins.mnemonic.startswith('b') and op:
            regs.pop(op[0],None)


def inventory(e):
    result=[]
    for start,end in e.code_ranges():
        ss=stores(e,start,end)
        for addr,off,val in ss:
            if val != 0xd123567a: continue
            near=[s for s in ss if abs(s[0]-addr)<=140]
            def value(offset):
                matches=[s for s in near if s[1]==offset]
                return min(matches,key=lambda s:abs(s[0]-addr))[2] if matches else None
            code=value(off-4)
            if code is None or code>=112: continue
            result.append(dict(command=code, function_address=f'0x{start:x}', cookie_store=f'0x{addr:x}',
                               method=e.byaddr.get(start,{}).get('demangled',f'cmd_{code:02d}_unknown'),
                               data_bytes=value(off+4), additional_bytes=value(off+8)))
        for addr,vals in templates(e,start,end):
            result.append(dict(command=vals[0],function_address=f'0x{start:x}',cookie_store=f'0x{addr:x}',
                               method=e.byaddr.get(start,{}).get('demangled',f'cmd_{vals[0]:02d}_unknown'),
                               data_bytes=vals[2],additional_bytes=vals[3]))
    return sorted(result,key=lambda r:(r['command'],r['function_address']))


def server_table(e):
    """Constructor stores two-word MIPS member pointers at this+4+8*ID."""
    fn=next(s for s in e.funcs if s['demangled']=='directv::dtvwm::IpcServer::IpcServer()')
    regs={};rows={}
    for ins in e.instructions(fn['address'],fn['address']+fn['size']):
        op=[p.strip() for p in ins.op_str.split(',')]
        if ins.mnemonic=='lw':
            m=re.fullmatch(r'(-?0x[0-9a-f]+|-?\d+)\(\$gp\)',op[1])
            regs[op[0]]=e.got.get(int(m[1],0),(None,''))[0] if m else None
        elif ins.mnemonic=='sw':
            m=re.fullmatch(r'(0x[0-9a-f]+|\d+)\(\$s0\)',op[1])
            if m:
                off=int(m[1],0);va=regs.get(op[0]);sym=e.byaddr.get(va,{})
                if off>=4 and (off-4)%8==0 and off<4+112*8 and 'IpcServer::' in sym.get('demangled',''):
                    rows[(off-4)//8]=dict(handler=sym['demangled'],handler_address=hex(va),store_address=hex(ins.address))
        elif ins.mnemonic not in ('nop','jr','sh','sb') and not ins.mnemonic.startswith('b') and op:
            regs.pop(op[0],None)
    return [dict(command=i,**rows[i]) for i in sorted(rows)]


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf');p.add_argument('--markdown',action='store_true');p.add_argument('--server',action='store_true')
    a=p.parse_args(); rows=server_table(Elf(a.elf)) if a.server else inventory(Elf(a.elf))
    if not a.markdown:print(json.dumps(rows,indent=2))
    elif a.server:
        print('| ID | Server handler | Address | Constructor store |\n|---:|---|---|---|')
        for r in rows:
            print(f"| {r['command']} | `{r['handler']}` | `{r['handler_address']}` | `{r['store_address']}` |")
    else:
        print('| ID | Client method | Address | Data bytes | Additional bytes |\n|---:|---|---|---:|---:|')
        for r in rows:
            def sz(v):return 'dynamic/unknown' if v is None else str(v)
            print(f"| {r['command']} | `{r['method']}` | `{r['function_address']}` | {sz(r['data_bytes'])} | {sz(r['additional_bytes'])} |")
