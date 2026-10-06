#!/usr/bin/env python3
"""Frigate camera items through the unchanged generic native UI."""
import os
from pathlib import Path
import subprocess
import sys
import time
ui=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ui.parent/'reboxd/tests'))
import frigate
h=frigate.Frigate('runTest');p=None
try:
    h.setUp();env={**os.environ,'REBOX_HOST_MANAGEMENT_SECRET_FILE':str(h.root/'module-state/management.secret')}
    p=subprocess.Popen([str(ui/'build/hr54-ui-host'),'--port',h.url.rsplit(':',1)[1],'--still'],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def keys(text):p.stdin.write(text.encode());p.stdin.flush();time.sleep(.3)
    time.sleep(.5)
    for _ in range(3):keys(' ')
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if h.get('/api/state')['playing']:break
        time.sleep(.1)
    state=h.get('/api/state');assert state['playing'] and state['source']=='frigate' and state['itemId']=='Front',state
    keys('x');assert not h.get('/api/state')['playing'];assert p.poll() is None
    print('PASS Frigate through generic native UI: dynamic Home, ordinary camera items, direct-plan playback and stop')
finally:
    if p:p.terminate();p.wait(timeout=10);p.stdin.close()
    h.doCleanups()
