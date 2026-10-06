#!/usr/bin/env python3
"""A real host core/package/RPC fixture for the compiled Android client tests."""
import json
from pathlib import Path
import secrets
import subprocess
import sys
root = Path(__file__).resolve().parents[3]
core = root / 'source/hr54-re/reboxd'
subprocess.run(['make', '-C', str(core), 'all'], check=True, stdout=subprocess.DEVNULL)
sys.path.insert(0, str(core / 'tests'))
import installer
h = installer.Installer('runTest')
try:
    h.setUp(); h.start()
    manifest = json.loads((core / '../modules/test-module/module.json').read_text())
    identity = 'android-future-' + secrets.token_hex(4)
    manifest.update(id=identity, name='Installed after APK compilation')
    url = h.archive(manifest=manifest)
    master = (h.root / 'module-state/management.secret').read_text()
    code, pairing = h.req('/api/management/pair/open', {}, token=master)
    assert code == 200
    # The Android code receives only a one-use pairing code, never the local secret.
    print(json.dumps({'receiver': h.url, 'package': url, 'id': identity, 'code': pairing['code']}), flush=True)
    sys.stdin.readline()
finally:
    h.doCleanups()
