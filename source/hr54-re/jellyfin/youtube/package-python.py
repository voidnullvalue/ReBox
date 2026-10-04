#!/usr/bin/env python3
"""Build sourceless zip imports. Host and target must share CPython magic."""
import argparse,pathlib,zipfile,py_compile,importlib.util,tempfile
p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('--stdlib',action='store_true');a=p.parse_args()
# CPython 3.14.0 magic (3627); .pyc marshaling is architecture/endian portable.
assert importlib.util.MAGIC_NUMBER==bytes.fromhex('2b0e0d0a')
with tempfile.TemporaryDirectory() as tmp,zipfile.ZipFile(a.output,'w',zipfile.ZIP_DEFLATED) as z:
 for f in a.source.rglob('*'):
  if not f.is_file():continue
  rel=f.relative_to(a.source)
  if any(x in ('test','tests','idlelib','tkinter','ensurepip','turtledemo','__pycache__') for x in rel.parts):continue
  if f.suffix=='.py':
   dest=pathlib.Path(tmp)/'module.pyc';py_compile.compile(str(f),cfile=str(dest),dfile=str(rel),invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH,doraise=True);z.write(dest,rel.with_suffix('.pyc'))
  elif not a.stdlib and f.suffix!='.pyc':z.write(f,rel)
print(a.output.stat().st_size)
