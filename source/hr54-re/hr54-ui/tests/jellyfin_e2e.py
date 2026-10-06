#!/usr/bin/env python3
"""Exercise the real generic native UI against the extracted Jellyfin module."""
import os
from pathlib import Path
import subprocess
import sys
import time
ui=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ui.parent/'reboxd/tests'))
import jellyfin
h=jellyfin.Jellyfin('runTest');p=None
try:
    h.setUp()
    env={**os.environ,'REBOX_HOST_MANAGEMENT_SECRET_FILE':str(h.root/'module-state/management.secret')}
    p=subprocess.Popen([str(ui/'build/hr54-ui-host'),'--port',h.url.rsplit(':',1)[1],'--still'],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def keys(s):p.stdin.write(s.encode());p.stdin.flush();time.sleep(.3)
    def wait(predicate):
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            if predicate():return
            time.sleep(.1)
        raise AssertionError('generic Jellyfin UI operation did not complete')
    time.sleep(.5)
    for _ in range(4):keys(' ')
    wait(lambda:h.get('/api/state')['playing'])
    assert h.get('/api/state')['itemId']=='vid1'
    keys('x');wait(lambda:not h.get('/api/state')['playing'])
    keys('g');keys('s');keys(' ');keys(' ');keys('ss ')
    keys('d ')
    wait(lambda:h.get(h.prefix+'/settings')['fields'][0]['value']==16000000)
    keys('ss ')
    wait(lambda:not h.get(h.prefix+'/status')['authenticated'])
    time.sleep(.3);keys('s ')
    wait(lambda:h.request(h.prefix+'/actions/auth.status',{},True)['pending'])
    h.approve.touch()
    wait(lambda:h.get(h.prefix+'/status')['authenticated'])
    assert p.poll() is None
    print('PASS Jellyfin through generic native UI: auth status, synthesized root, opaque browse/play, module quality settings, disconnect, displayed auth action and automatic approval polling')
finally:
    if p:p.terminate();p.wait(timeout=10);p.stdin.close()
    h.doCleanups()
