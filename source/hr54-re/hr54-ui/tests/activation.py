#!/usr/bin/env python3
"""Real backend boot selection without touching receiver services."""
from pathlib import Path
import subprocess,tempfile,os,socket,time,json,urllib.request,signal
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hr54-native-activation-') as td:
    base=Path(td); persist=base/'jellyfin'; menu=base/'native-menu'; menu.mkdir()
    exe=base/'backend'
    subprocess.run(['cc','-O1','-o',str(exe),str(root.parent/'jellyfin/remote/hr54_jf.c')],check=True)
    helper=menu/'bootstrap.sh'; called=menu/'called'
    helper.write_text('#!/bin/sh\nprintf started > "'+str(called)+'"\n'); helper.chmod(0o700)
    for phase in range(3):
        if phase==1: (menu/'ENABLED').touch()
        if phase==2: (menu/'ACTIVATED').touch()
        sock=socket.socket(); sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]; sock.close()
        p=subprocess.Popen([str(exe),str(root.parent/'jellyfin/remote/static'),'127.0.0.1','8096',str(port),'--no-launcher'],env=dict(os.environ,JF_PERSIST_ROOT=str(persist)),start_new_session=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            end=time.monotonic()+8
            while True:
                try:
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/system/status',timeout=1) as r: status=json.load(r)
                    break
                except OSError:
                    assert time.monotonic()<end; time.sleep(.05)
            assert status['frontend']==('native' if phase==2 else 'legacy'), status
            assert status['mediaBusy'] is False
            if phase==2:
                while not called.exists() and time.monotonic()<end: time.sleep(.01)
                assert called.read_text()=='started'
            else: assert not called.exists()
        finally:
            os.killpg(p.pid,signal.SIGTERM); p.wait(timeout=3)
print('PASS real backend automatic native activation requires both markers and launches fixed helper')
