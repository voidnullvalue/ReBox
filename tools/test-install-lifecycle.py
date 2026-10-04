#!/usr/bin/env python3
"""Isolated activation/rollback tests; no receiver, disk, or root privilege."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
idle = '{"ok":true,"ready":true,"frontend":"legacy","doomRunning":false,"mediaBusy":false}'

def executable(path, text):
    path.write_text('#!/bin/sh\nset -eu\n'+text+'\n')
    path.chmod(0o700)

for scenario in ['wrong_firmware', 'wrong_pid', 'busy', 'malformed', 'success']:
    with tempfile.TemporaryDirectory(prefix='rebox-lifecycle-') as td:
        directory = Path(td)
        base = directory/'persist'
        menu = base/'native-menu'
        registry = directory/'registry'
        commands = directory/'commands'
        for p in [menu/'backup', base/'jellyfin/bin', registry, commands]:
            p.mkdir(parents=True, exist_ok=True)
        stock = directory/'stock.car'
        shutil.copyfile(root/'source/hr54-re/extracted/sdb4-rootfs/opt/dtv/dtv.car', stock)
        (base/'fixture').write_text('hash-checked payload fixture')
        (base/'payload.md5').write_text(hashlib.md5((base/'fixture').read_bytes()).hexdigest()+'  fixture\n')
        (base/'stock-firmware.md5').write_text(('0'*32 if scenario=='wrong_firmware' else '882124071cfe3ac1740af18161995dcf')+'  '+str(stock)+'\n')
        (registry/'Context-Main.str').write_text('com.directv.druid.DruidMain')
        (registry/'Auto-Start.str').write_text('true')
        (base/'rebox.conf').write_text('JELLYFIN_IPV4=192.168.1.2\nJELLYFIN_PORT=8096\n')
        backend = base/'jellyfin/bin/hr54-jf'
        shutil.copyfile('/bin/sleep', backend)
        backend.chmod(0o700)
        process = subprocess.Popen([str(backend),'60'])
        try:
            (base/'jellyfin/jf.pid').write_text(('not-a-pid' if scenario=='wrong_pid' else str(process.pid))+'\n')
            response = idle.replace('"mediaBusy":false','"mediaBusy":true') if scenario=='busy' else '{}' if scenario=='malformed' else idle
            (directory/'readiness').write_text(response)
            executable(commands/'wget', 'cat "$REBOX_TEST_READINESS"')
            executable(commands/'id', '[ "$1" = -u ]; printf "0\\n"')
            executable(base/'rebox-start.sh', 'printf "started\\n" > "$REBOX_TEST_STARTED"')
            executable(menu/'hr54-ui', '[ "$1" = --check-api ] && [ "$2" = --idle ]')
            environment = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'], REBOX_TEST_READINESS=str(directory/'readiness'), REBOX_TEST_STARTED=str(directory/'started'))
            def run(name, confirmation):
                text = (root/'tools'/name).read_text().replace('/var/hr54-persist',str(base)).replace('/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID',str(registry)).replace('/opt/dtv/dtv.car',str(stock))
                script = directory/name
                script.write_text(text)
                return subprocess.run(['sh',str(script),confirmation],env=environment,capture_output=True,text=True,timeout=20)
            result = run('activate-native.sh','ACTIVATE_NATIVE_ON_SUPPORTED_STOCK_HR54')
            if scenario!='success':
                assert result.returncode!=0 and not (menu/'ENABLED').exists(), result.stdout+result.stderr
                assert process.poll() is None and (registry/'Auto-Start.str').read_text()=='true'
                print('PASS activation refuses',scenario,'before markers/process termination')
                continue
            assert result.returncode==0, result.stdout+result.stderr
            process.wait(timeout=2)
            assert (directory/'started').exists() and (menu/'ENABLED').exists()
            backup = Path((menu/'rebox-rollback-path').read_text().strip())
            assert (backup/'dtv.car.stock').read_bytes()==stock.read_bytes()
            assert (backup/'Auto-Start.str').read_text()=='true'
            (registry/'Auto-Start.str').write_text('false')
            restored = run('deactivate-native.sh','RESTORE_STOCK_PRESENTATION')
            assert restored.returncode==0, restored.stdout+restored.stderr
            assert (registry/'Auto-Start.str').read_text()=='true'
            assert not (menu/'ENABLED').exists() and backup.exists()
            assert any(p.name.startswith('rebox-disabled.') for p in (menu/'backup').iterdir())
            print('PASS activation backup, exact owned-process shutdown, start, and non-destructive stock presentation rollback')
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
