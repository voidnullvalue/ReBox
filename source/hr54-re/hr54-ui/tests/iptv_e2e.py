#!/usr/bin/env python3
"""IPTV's groups/channels are exercised through the unchanged generic shell."""
import os
from pathlib import Path
import subprocess
import sys
import time
ui = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ui.parent/'reboxd/tests'))
import iptv
h = iptv.Iptv('runTest'); p = None
try:
    h.setUp()
    env = {**os.environ, 'REBOX_HOST_MANAGEMENT_SECRET_FILE': str(h.root/'module-state/management.secret')}
    p = subprocess.Popen([str(ui/'build/hr54-ui-host'), '--port', h.url.rsplit(':', 1)[1], '--still'], env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def keys(text): p.stdin.write(text.encode()); p.stdin.flush(); time.sleep(.3)
    time.sleep(.5)
    # Home -> module root -> All channels -> channel details -> play.
    for _ in range(4): keys(' ')
    h.wait(lambda: h.get('/api/state')['playing'])
    assert h.get('/api/state')['itemId'] == h.channels['Live']
    assert h.get('/api/state')['source'] == 'iptv'
    keys('x'); h.wait(lambda: not h.get('/api/state')['playing'])
    keys('g'); keys(' '); keys('b'); keys('s '); keys(' '); keys(' ')
    h.wait(lambda: h.get('/api/state')['playing'])
    assert h.get('/api/state')['itemId'] == h.channels['Live']
    keys('x'); h.wait(lambda: not h.get('/api/state')['playing'])
    assert p.poll() is None
    print('PASS IPTV through generic native UI: dynamic Home, All channels, module group folder, opaque channel playback and stop')
finally:
    if p: p.terminate(); p.wait(timeout=10); p.stdin.close()
    h.doCleanups()
