#!/usr/bin/env python3
"""Offline ELF32/MSB MIPS inspection. Requires capstone; never executes firmware.

GOT offsets come from readelf -A (including MIPS lazy stubs). Calls through
unresolved object/vtable pointers are deliberately left unresolved.
"""
import argparse
import bisect
import json
import re
import struct
import subprocess
from pathlib import Path
import capstone


class Elf:
    def __init__(self, path):
        self.path = str(path)
        self.data = Path(path).read_bytes()
        if self.data[:6] != b'\x7fELF\x01\x02':
            raise ValueError('expected ELF32 big endian')
        off = self.u32(28)
        size, count = struct.unpack_from('>HH', self.data, 42)
        self.loads = [struct.unpack_from('>8I', self.data, off+i*size)
                      for i in range(count)]
        self.loads = [p for p in self.loads if p[0] == 1]
        self.symbols = []
        for line in subprocess.check_output(['readelf', '-Ws', self.path], text=True).splitlines():
            m = re.match(r'\s*(\d+):\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*)', line)
            if m:
                idx, va, sz, kind, bind, vis, ndx, name = m.groups()
                self.symbols.append(dict(index=int(idx), address=int(va, 16), size=int(sz),
                                         kind=kind, section=ndx, name=name.split('@')[0]))
        names = [s['name'] for s in self.symbols]
        demangled = subprocess.check_output(['c++filt'], input='\n'.join(names)+'\n', text=True).splitlines()
        for s, name in zip(self.symbols, demangled):
            s['demangled'] = name
        self.byaddr = {s['address']: s for s in self.symbols if s['address']}
        self.relocations = {}
        for line in subprocess.check_output(['readelf', '-rW', self.path], text=True).splitlines():
            m = re.match(r'([0-9a-f]+)\s+[0-9a-f]+\s+R_MIPS_(?:REL32|32)\s+([0-9a-f]+)\s+(\S+)', line)
            if m:
                self.relocations[int(m[1],16)] = (int(m[2],16), m[3].split('@')[0])
        self.funcs = sorted({s['address']: s for s in self.symbols
                             if s['kind'] == 'FUNC' and s['section'] != 'UND' and s['size']}.values(),
                            key=lambda s: s['address'])
        self.starts = [s['address'] for s in self.funcs]
        self.got = {}
        for line in subprocess.check_output(['readelf', '-AW', self.path], text=True).splitlines():
            m = re.match(r'\s*([0-9a-f]+)\s+(-?\d+)\(gp\)\s+([0-9a-f]+)(.*)', line)
            if m:
                self.got[int(m[2])] = (int(m[3], 16), m[4].strip())
        self.cs = capstone.Cs(capstone.CS_ARCH_MIPS,
                              capstone.CS_MODE_MIPS32 | capstone.CS_MODE_BIG_ENDIAN)
        self.cs.skipdata = True

    def code_ranges(self):
        # Recover PIC function starts in stripped code from the canonical
        # three-instruction gp setup. Also retain symbolized leaf functions.
        starts = set(self.starts)
        for _, off, base, _, length, _, flags, _ in self.loads:
            if not flags & 1: continue
            for pos in range(off, off+length-11, 4):
                a, b, c = struct.unpack_from('>3I', self.data, pos)
                if a & 0xffff0000 == 0x3c1c0000 and b & 0xffff0000 == 0x279c0000 and c == 0x0399e021:
                    starts.add(base+pos-off)
        for _, off, base, _, length, _, flags, _ in self.loads:
            if not flags & 1: continue
            ss = sorted(s for s in starts if base <= s < base+length)
            for i, start in enumerate(ss):
                yield start, ss[i+1] if i+1 < len(ss) else base+length

    def u32(self, off):
        return struct.unpack_from('>I', self.data, off)[0]

    def read(self, va, size):
        for _, off, base, _, length, *_ in self.loads:
            if base <= va and va+size <= base+length:
                return self.data[off+va-base:off+va-base+size]
        raise ValueError(f'unmapped address {va:#x}+{size:#x}')

    def string(self, va):
        try:
            for _, off, base, _, length, *_ in self.loads:
                if base <= va < base+length:
                    b = self.data[off+va-base:off+length].split(b'\0', 1)[0]
                    if b and len(b) < 512 and all(32 <= c < 127 or c in (9, 10, 13) for c in b):
                        return b.decode('ascii')
        except ValueError:
            pass
        return None

    def owner(self, va):
        i = bisect.bisect_right(self.starts, va)-1
        if i >= 0 and va < self.funcs[i]['address']+self.funcs[i]['size']:
            return self.funcs[i]['demangled']
        return '<unsymbolized>'

    def instructions(self, start, end):
        return self.cs.disasm(self.read(start, end-start), start)

    def annotate(self, start, end):
        regs = {}
        for ins in self.instructions(start, end):
            ops = [x.strip() for x in ins.op_str.split(',')]
            note = ''
            if ins.mnemonic == 'lw' and len(ops) == 2:
                m = re.fullmatch(r'(-?0x[0-9a-f]+|-?\d+)\(\$gp\)', ops[1])
                if m:
                    v, desc = self.got.get(int(m[1], 0), (None, '?'))
                    regs[ops[0]] = v
                    note = f'GOT {v:#x} {desc}' if v is not None else 'GOT ?'
            elif ins.mnemonic == 'lui' and len(ops) == 2:
                regs[ops[0]] = int(ops[1], 0) << 16
            elif ins.mnemonic in ('addiu', 'ori') and len(ops) == 3:
                v = regs.get(ops[1], 0 if ops[1] == '$zero' else None)
                if v is not None:
                    regs[ops[0]] = ((v+int(ops[2], 0)) if ins.mnemonic == 'addiu'
                                      else v | int(ops[2], 0)) & 0xffffffff
                    st = self.string(regs[ops[0]])
                    if st:
                        note = f'string {regs[ops[0]]:#x} {st!r}'
                else:
                    regs.pop(ops[0], None)
            elif ins.mnemonic == 'move' and len(ops) == 2:
                regs[ops[0]] = regs.get(ops[1])
            elif ins.mnemonic == 'jalr':
                target = regs.get(ops[-1])
                if target is not None:
                    s = self.byaddr.get(target)
                    note = f'call {target:#x} '+(s['demangled'] if s else self.owner(target))
                # Linear tracking, not full CFG; no conclusions about call arguments.
                for r in ('$v0', '$v1', '$a0', '$a1', '$a2', '$a3', '$t9'):
                    regs.pop(r, None)
            elif ins.mnemonic in ('jal', 'j', 'b'):
                try:
                    target = int(ops[-1], 0)
                    note = f'target {self.owner(target)}'
                except ValueError:
                    pass
            elif ins.mnemonic not in ('sw', 'sh', 'sb', 'nop', 'jr') and ops and ops[0].startswith('$'):
                # Do not carry values through arbitrary arithmetic or loads.
                regs.pop(ops[0], None)
            yield ins, note


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf')
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('symbols'); s.add_argument('--match', default=''); s.add_argument('--json', action='store_true')
    s = sub.add_parser('disasm'); s.add_argument('--match'); s.add_argument('--start', type=lambda s:int(s, 0)); s.add_argument('--end', type=lambda s:int(s, 0))
    s = sub.add_parser('tables'); s.add_argument('--match', default='vtable for')
    s = sub.add_parser('calls'); s.add_argument('--match', default='')
    s = sub.add_parser('imports'); s.add_argument('--match', required=True)
    s = sub.add_parser('xrefs'); s.add_argument('--match', required=True)
    s = sub.add_parser('switch'); s.add_argument('address', type=lambda s:int(s,0)); s.add_argument('count', type=int); s.add_argument('--gp', type=lambda s:int(s,0), default=0)
    a = p.parse_args(); e = Elf(a.elf)
    if a.mode == 'imports':
        # Search GOT-load instructions only, then annotate a small call window.
        # Works on stripped PIC clients without disassembling all large code.
        seen=set()
        for offset,(va,desc) in e.got.items():
            name=e.byaddr.get(va,{}).get('demangled',desc)
            if not re.search(a.match,name):continue
            for _,fo,base,_,size,_,flags,_ in e.loads:
                if not flags&1:continue
                for reg in range(32):
                    pattern=struct.pack('>I',0x8f800000|(reg<<16)|(offset&0xffff))
                    pos=fo
                    while True:
                        pos=e.data.find(pattern,pos,fo+size)
                        if pos<0:break
                        start=base+pos-fo;pos+=4
                        if start%4:continue
                        for ins,note in e.annotate(start,min(start+128,base+size)):
                            if ins.mnemonic=='jalr' and re.search(a.match,note) and ins.address not in seen:
                                seen.add(ins.address);print(f'load={start:08x} call={ins.address:08x} caller={e.owner(ins.address)} {note}')
    elif a.mode == 'switch':
        for i in range(a.count):
            raw = struct.unpack('>I',e.read(a.address+i*4,4))[0]
            target = (raw+a.gp)&0xffffffff
            print(f'{i:3d} {raw:08x} -> {target:08x} {e.owner(target)}')
    elif a.mode == 'xrefs':
        for start,end in e.code_ranges():
            for ins,note in e.annotate(start,end):
                if 'string ' in note and re.search(a.match,note):
                    print(f'function={start:08x} xref={ins.address:08x} {note}')
    elif a.mode == 'symbols':
        syms = [s for s in e.symbols if re.search(a.match, s['demangled'])]
        if a.json: print(json.dumps(syms, indent=2))
        else:
            for s in syms: print(f"{s['address']:08x} {s['size']:6d} {s['section']:>3} {s['demangled']}")
    elif a.mode == 'tables':
        for s in e.symbols:
            if s['section'] != 'UND' and s['size'] and re.search(a.match, s['demangled']):
                print(f"\n{s['demangled']} @ {s['address']:#x}")
                for off in range(0, s['size']-3, 4):
                    v = struct.unpack('>I', e.read(s['address']+off, 4))[0]
                    rel = e.relocations.get(s['address']+off)
                    if rel:
                        v = (v+rel[0])&0xffffffff
                    target = e.byaddr.get(v, {}).get('demangled', rel[1] if rel else '')
                    print(f'{off:04x} {v:08x} {target}')
    else:
        ranges = [(s['address'], s['address']+s['size'], s['demangled']) for s in e.funcs
                  if not a.match or re.search(a.match, s['demangled'])]
        if a.mode == 'disasm' and a.start is not None:
            if a.end is None: p.error('--end required with --start')
            ranges = [(a.start, a.end, e.owner(a.start))]
        for start, end, name in ranges:
            print(f'\n{name} @ {start:#x}')
            for ins, note in e.annotate(start, end):
                if a.mode == 'calls' and ins.mnemonic not in ('jal', 'jalr'): continue
                print(f'{ins.address:08x} {ins.mnemonic:8s} {ins.op_str:45s}'+(' # '+note if note else ''))


if __name__ == '__main__':
    main()
