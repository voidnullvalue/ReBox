#!/usr/bin/env python3
"""The manifest opens search; the shell has no provider-specific dispatch."""
import os
from pathlib import Path
import subprocess
import sys
import time
ui=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ui.parent/'reboxd/tests'))
import youtube
h=youtube.Youtube('runTest');p=None
try:
    h.setUp()
    env={**os.environ,'REBOX_HOST_MANAGEMENT_SECRET_FILE':str(h.root/'module-state/management.secret')}
    p=subprocess.Popen([str(ui/'build/hr54-ui-host'),'--port',h.url.rsplit(':',1)[1],'--still'],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def keys(s):p.stdin.write(s.encode());p.stdin.flush();time.sleep(.3)
    time.sleep(.5);keys(' ')
    # Keyboard begins at a: type it, then submit using the generic Search key.
    keys(' ');keys('d'*44);keys(' ');keys(' ');keys(' ')
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if h.get('/api/state')['playing']:break
        time.sleep(.1)
    state=h.get('/api/state');assert state['playing'] and state['source']=='youtube' and state['itemId']=='jNQXAC9IVRw',state
    keys('x');assert not h.get('/api/state')['playing'];assert p.poll() is None
    print('PASS YouTube through generic native UI: manifest search-first Home, keyboard, normalized search results, opaque playback and stop')
finally:
    if p:p.terminate();p.wait(timeout=10);p.stdin.close()
    h.doCleanups()
