#!/usr/bin/env python3
"""An unknown native-app module through the real generic shell; no EGL mock."""
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
ui=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ui.parent/'reboxd/tests'))
import native_app
h=native_app.Native('runTest');p=None
try:
    h.setUp();identity='future-native-'+secrets.token_hex(3);h.install_native(identity);h.start()
    env={**os.environ,'REBOX_HOST_MANAGEMENT_SECRET_FILE':str(h.root/'module-state/management.secret')}
    p=subprocess.Popen([str(ui/'build/hr54-ui-host'),'--port',h.url.rsplit(':',1)[1],'--still'],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def keys(text):p.stdin.write(text.encode());p.stdin.flush();time.sleep(.3)
    def wait(predicate):
        end=time.monotonic()+8
        while time.monotonic()<end:
            if predicate():return
            time.sleep(.1)
        raise AssertionError('native-app UI operation timed out')
    time.sleep(.5);keys(' ');wait(lambda:h.get('/api/system/status')['nativeModule']==identity)
    keys('g');wait(lambda:not h.get('/api/system/status')['mediaBusy'])
    keys(' ');wait(lambda:h.get('/api/system/status')['nativeModule']==identity)
    keys('x');wait(lambda:not h.get('/api/system/status')['mediaBusy'])
    assert p.poll() is None
    print('PASS unknown native-app through generic UI: manifest presentation, start, MENU return, restart and Stop without rebuilding')
finally:
    if p:p.terminate();p.wait(timeout=10);p.stdin.close()
    h.doCleanups()
