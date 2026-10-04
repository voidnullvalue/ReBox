#!/usr/bin/env python3
"""Fail if frontend source gains a device/source integration outside API/input/presentation."""
from pathlib import Path
import re,subprocess
root=Path(__file__).resolve().parents[1]
for path in [root/'main.c',*root.glob('api/*.c'),*root.glob('apps/*.c'),*root.glob('input/*.c'),*root.glob('ui/*.c')]:
 text=path.read_text()
 forbidden=r'\b(?:system|execl|execv|fork|popen|dlopen|kill)\s*\(|UCONNECT|BinaryIPC|DirectTest|yt-dlp|["\']/(?:dev|opt|var)/(?!.*hr54-ui\.lock)|https?://(?!127\.0\.0\.1)'
 assert not re.search(forbidden,text),f'API boundary violation in {path}'
 if path.parent.name in ('apps','ui'):
  assert not re.search(r'\b(?:socket|connect|recv|send)\s*\(',text),f'transport in screen/controller {path}'
 if path.parent.name=='ui':assert 'api_send(' not in text,f'HTTP construction in renderer {path}'
binary=root/'build/hr54-ui'
if binary.exists():
 deps=subprocess.check_output(['readelf','-d',str(binary)],text=True)
 for name in ('libuserinput','libdtvwm','libuconnect','libbinaryipc','libwebkit','libcurl'):
  assert name not in deps,f'forbidden device dependency {name}'
print('PASS frontend API boundary, screen transport isolation, receiver link audit')
