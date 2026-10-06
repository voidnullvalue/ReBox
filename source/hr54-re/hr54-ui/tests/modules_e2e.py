#!/usr/bin/env python3
"""Install an unknown executable through the actual native keyboard and HTTP API.

One running frontend binary observes install/disable/enable/uninstall. The
receiver decoder alone is mocked by reboxd-host; packaging/RPC/UI are real.
"""
import json
import io
from PIL import Image
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
ui = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ui.parent / 'reboxd/tests'))
import installer

def wait(predicate, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError('native module operation did not complete')

h = installer.Installer('runTest')
h.setUp()
p = None
try:
    h.start()
    manifest = json.loads((ui.parent / 'modules/test-module/module.json').read_text())
    identity = 'future-' + secrets.token_hex(5)
    manifest['id'] = identity
    manifest['name'] = 'Installed after compilation'
    manifest['icon']='icon.png'
    icon=io.BytesIO();Image.new('RGBA',(128,128),(220,30,60,255)).save(icon,format='PNG')
    url = h.archive(manifest=manifest,extra=[('icon.png',icon.getvalue(),0o600,'0')])
    frame = h.base / 'frame.rgba'
    reference = h.base / 'empty.rgba'
    subprocess.run([str(ui / 'build/hr54-ui-host'), '--preview', 'home-zero', '--output', str(reference)], check=True)
    empty = reference.read_bytes()
    env = {**os.environ, 'REBOX_HOST_MANAGEMENT_SECRET_FILE': str(h.root / 'module-state/management.secret')}
    p = subprocess.Popen([str(ui / 'build/hr54-ui-host'), '--port', h.url.rsplit(':', 1)[1], '--still', '--output', str(frame)], env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def keys(text):
        p.stdin.write(text.encode()); p.stdin.flush()
    def empty_home(): return frame.exists() and frame.read_bytes() == empty
    def modules(): return h.get('/api/modules')['modules']
    def enter(text, url_mode=False, clear=False):
        letters = 'abcdefghijklmnopqrstuvwxyz0123456789:/.-_?&=%#~+@' if url_mode else "abcdefghijklmnopqrstuvwxyz0123456789-.'?"
        cells = 50 if url_mode else 40
        focus = 0
        def move(target):
            nonlocal focus
            distance = (target-focus) % (cells+10)
            keys('d'*distance if distance <= (cells+10)//2 else 'a'*((cells+10)-distance))
            focus = target
        if clear: move(cells+3); keys(' ')
        for char in text: move(letters.index(char)); keys(' ')
        move(cells+4); keys(' ')
    wait(empty_home)
    # Home -> Settings -> Modules -> Add URL; type using the production keyboard.
    keys('s'); time.sleep(.1); keys(' '); time.sleep(.2); keys(' '); time.sleep(.1)
    enter(url, url_mode=True, clear=True)
    wait(lambda: any(m['id'] == identity and m['healthy'] for m in modules()), 30)
    time.sleep(.5); keys('g'); time.sleep(.3)
    def icon_visible():
        raw=frame.read_bytes();offset=(204*720+360)*4
        return len(raw)>offset+4 and raw[offset:offset+4]==bytes((220,30,60,255))
    wait(icon_visible)
    # Root Folder A -> Item 2 -> details -> play, all with opaque IDs.
    for _ in range(4): keys(' '); time.sleep(.35)
    wait(lambda: h.get('/api/state')['playing'])
    state = h.get('/api/state')
    assert state['source'] == identity and state['itemId'] == 'item-2', state
    keys('x'); wait(lambda: not h.get('/api/state')['playing']); time.sleep(.2)
    keys('g'); time.sleep(.1); keys(' '); time.sleep(.35); keys('i'); time.sleep(.1)
    enter('foo'); time.sleep(.4); keys(' '); time.sleep(.1); keys(' ')
    wait(lambda: h.get('/api/state')['playing'])
    assert h.get('/api/state')['itemId'] == 'item-1'
    keys('x'); wait(lambda: not h.get('/api/state')['playing']); time.sleep(.2)
    def detail():
        keys('g'); time.sleep(.1); keys('s'); time.sleep(.1); keys(' '); time.sleep(.3); keys(' '); time.sleep(.1)
    detail(); keys(' ')
    wait(lambda: not modules()[0]['enabled'])
    keys('g'); wait(empty_home)
    detail(); keys(' ')
    wait(lambda: modules()[0]['enabled'] and modules()[0]['healthy'])
    time.sleep(.3)
    detail(); keys('s ')
    wait(lambda: modules() == [])
    keys('g'); wait(empty_home)
    assert p.poll() is None
    print('PASS unknown URL package through native keyboard, dynamic Home and downloaded icon, opaque folder/item/search/play, disable, re-enable, uninstall without restarting/rebuilding frontend')
finally:
    if p:
        p.terminate(); p.wait(timeout=10); p.stdin.close()
    h.doCleanups()
