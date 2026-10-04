#!/bin/bash
set -eu
test -f /tmp/hr54-youtube-build/deps/lib/libssl.a
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
/tmp/hr54-zig/zig cc -target mips-linux-musleabi -mcpu=mips32 -O2 -c "$src/atomic-query.c" -o /tmp/hr54-youtube-build/atomic-query.o
/tmp/hr54-zig/zig ar r /tmp/hr54-youtube-build/deps/lib/libcrypto.a /tmp/hr54-youtube-build/atomic-query.o
CC='/tmp/hr54-zig/zig cc -target mips-linux-musleabi -mcpu=mips32' AR='/tmp/hr54-zig/zig ar' RANLIB='/tmp/hr54-zig/zig ranlib' CFLAGS='-O2 -Wno-error=date-time' CPPFLAGS='-I/tmp/hr54-youtube-build/deps/include' LDFLAGS='-static -s -L/tmp/hr54-youtube-build/deps/lib' ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no ./configure --host=mips-linux-musl --build=x86_64-linux-gnu --with-build-python=/usr/bin/python3.14 --prefix=/var/hr54-persist/jellyfin/youtube/python --without-ensurepip --disable-test-modules --with-openssl=/tmp/hr54-youtube-build/deps >../python-configure2.log 2>&1
python3 - <<'PY'
from pathlib import Path
wanted='array _asyncio _bisect _csv _heapq _json _pickle _queue _random _struct math cmath _statistics binascii zlib _md5 _sha1 _sha2 _sha3 _blake2 _hmac pyexpat _elementtree unicodedata fcntl grp mmap _posixsubprocess resource select _socket _ssl _hashlib termios'.split()
lines=Path('Modules/Setup.stdlib').read_text().splitlines()
selected=[line for line in lines if line and line.split()[0] in wanted]
Path('Modules/Setup.local').write_text('*static*\n'+'\n'.join(selected)+'\n*disabled*\n_ctypes _bz2 _lzma _zstd _sqlite3 _curses _curses_panel readline _decimal _dbm _gdbm\n')
PY
python3 - <<'PY'
from pathlib import Path
p=Path('Makefile');t=p.read_text().replace('LOCALMODLIBS=','HR54_LOCALMODLIBS=',1)
t+='\nLOCALMODLIBS = $(sort $(filter %.o,$(HR54_LOCALMODLIBS))) $(filter-out %.o,$(HR54_LOCALMODLIBS))\n'
p.write_text(t)
PY
make -j4 python >../python-build.log 2>&1
make pybuilddir.txt
