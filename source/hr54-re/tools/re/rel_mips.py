#!/usr/bin/env python3
"""Offline ELF32/MSB ET_REL disassembly with section-relative relocations.

Addresses in modules are section offsets, never receiver runtime addresses.
Relocations are annotated, not applied; unresolved indirect calls stay unknown.
"""
import argparse
import re
import struct
from pathlib import Path
import capstone


class RelElf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:6] != b'\x7fELF\x01\x02' or self.u16(16) != 1:
            raise ValueError('expected ELF32 big-endian ET_REL')
        off, size, count, strings = self.u32(32), self.u16(46), self.u16(48), self.u16(50)
        self.sections = [struct.unpack_from('>10I', self.data, off+i*size) for i in range(count)]
        shstr = self.section_data(strings)
        self.names = [self.cstring(shstr, s[0]) for s in self.sections]
        self.symbols = []
        self.relocs = {}
        for si, s in enumerate(self.sections):
            if s[1] != 2:
                continue
            names = self.section_data(s[6])
            syms = []
            for pos in range(s[4], s[4]+s[5], s[9]):
                n, va, length, info, _, section = struct.unpack_from('>IIIBBH', self.data, pos)
                syms.append(dict(name=self.cstring(names, n), address=va, size=length,
                                 kind=info & 15, section=section))
            self.symbols.extend(syms)
            for r in self.sections:
                if r[1] == 9 and r[6] == si:
                    for pos in range(r[4], r[4]+r[5], 8):
                        at, info = struct.unpack_from('>II', self.data, pos)
                        target = syms[info >> 8]
                        self.relocs.setdefault((r[7], at), []).append((info & 255, target))
        self.cs = capstone.Cs(capstone.CS_ARCH_MIPS,
                              capstone.CS_MODE_MIPS32 | capstone.CS_MODE_BIG_ENDIAN)

    def u16(self, off):
        return struct.unpack_from('>H', self.data, off)[0]

    def u32(self, off):
        return struct.unpack_from('>I', self.data, off)[0]

    @staticmethod
    def cstring(data, off):
        return data[off:].split(b'\0', 1)[0].decode('ascii', errors='replace')

    def section_data(self, section):
        s = self.sections[section]
        return self.data[s[4]:s[4]+s[5]]

    def disasm(self, section, start, size):
        for ins in self.cs.disasm(self.section_data(section)[start:start+size], start):
            notes = []
            for kind, target in self.relocs.get((section, ins.address), []):
                ndx = target['section']
                sec = self.names[ndx] if ndx < len(self.names) else str(ndx)
                addend = (struct.unpack('>I', ins.bytes)[0] & 0x3ffffff) << 2 if kind == 4 else None
                where = target['address']+(addend or 0)
                name = target['name'] or sec
                if kind == 4 and not target['name']:
                    owner = next((s for s in self.symbols if s['section'] == ndx and
                                  s['kind'] == 2 and s['address'] <= where < s['address']+s['size']), None)
                    if owner:
                        name = owner['name']
                notes.append(f'R_MIPS_{kind} {name} ({sec}+0x{where:x})')
            yield ins, '; '.join(notes)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf'); p.add_argument('--match', default='')
    p.add_argument('--section', default='.text'); p.add_argument('--start', type=lambda v: int(v, 0))
    p.add_argument('--end', type=lambda v: int(v, 0)); p.add_argument('--symbols', action='store_true')
    a = p.parse_args(); e = RelElf(a.elf)
    if a.symbols:
        for s in e.symbols:
            if re.search(a.match, s['name']) and s['section'] < len(e.names):
                print(f"{e.names[s['section']]}+0x{s['address']:x} {s['size']} {s['name']}")
        return
    if a.start is not None:
        if a.end is None:
            p.error('--end required with --start')
        ranges = [(e.names.index(a.section), a.start, a.end-a.start, 'inferred range')]
    else:
        ranges = [(s['section'], s['address'], s['size'], s['name']) for s in e.symbols
                  if s['kind'] == 2 and s['size'] and re.search(a.match, s['name'])]
    for section, start, size, name in ranges:
        print(f'\n{name} @ {e.names[section]}+0x{start:x}')
        for ins, note in e.disasm(section, start, size):
            print(f'{ins.address:08x} {ins.mnemonic:8s} {ins.op_str:40s} # {note}')


if __name__ == '__main__':
    main()
