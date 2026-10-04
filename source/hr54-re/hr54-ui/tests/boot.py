#!/usr/bin/env python3
"""Exercise the production bootstrap with isolated paths and stock-command doubles."""
from pathlib import Path
import os, shutil, subprocess, tempfile, json, http.server, threading, time, hashlib

root = Path(__file__).resolve().parents[1]
stock = root.parent / 'extracted/sdb4-rootfs/opt/dtv/dtv.car'
policy = Path(os.environ.get('HR54_TEST_POLICY', root / 'build/dtv-menu-policy.car'))

with tempfile.TemporaryDirectory(prefix='hr54-native-boot-') as td:
    base = Path(td)
    menu = base / 'native-menu'
    menu.mkdir()
    overlays = base / 'overlays'
    overlays.mkdir()
    commands = base / 'commands'
    commands.mkdir()
    calls = base / 'calls'
    vendor = base / 'dtv.car'
    source = (root / 'bootstrap.sh').read_text()
    source = source.replace('/var/hr54-persist/native-menu', str(menu))
    source = source.replace('/var/hr54-persist/overlays', str(overlays))
    source = source.replace('/opt/dtv/dtv.car', str(vendor))
    source = source.replace('/opt/ui_assets/assetspack/language/englishtext.txt', str(base/'language.txt'))
    source = source.replace('/tmp/hr54-native-bootstrap.v2.attempted', str(base/'attempted'))
    script = base / 'bootstrap.sh'
    script.write_text(source)
    shutil.copyfile(policy, menu/'dtv-menu-policy.car')
    (menu/'policy.md5').write_text(hashlib.md5(policy.read_bytes()).hexdigest()+'\n')
    for flag in ['ENABLED', 'ACTIVATED']:
        (menu/flag).touch()
    def executable(path, body):
        path.write_text('#!/bin/sh\nset -eu\n' + body + '\n')
        path.chmod(0o700)
    executable(commands/'pidof', '[ "$1" = hr54-ui ] && exit 1; printf "1\\n"')
    executable(commands/'sleep', '/bin/sleep 0.01')
    executable(commands/'dt', 'printf "dt %s\\n" "$*" >> "$BOOT_CALLS"')
    executable(commands/'mount', 'printf "mount\\n" >> "$BOOT_CALLS"; cp "$2" "$3"')
    executable(commands/'umount', 'printf "umount\\n" >> "$BOOT_CALLS"; cp "$BOOT_STOCK" "$1"')
    executable(menu/'hr54-ui', '''printf 'ui %s\n' "$*" >> "$BOOT_CALLS"
case "$1" in
 --check-api) [ ! -f "$BOOT_BASE/backend-down" ];;
 --probe-input) [ ! -f "$BOOT_BASE/input-rejected" ];;
 *) exit 2;;
esac''')
    executable(menu/'supervisor.sh', 'printf "supervisor\\n" >> "$BOOT_CALLS"')
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'],
               BOOT_CALLS=str(calls), BOOT_STOCK=str(stock), BOOT_BASE=str(base),
               HR54_BOOT_WAIT_SECONDS='1')
    def reset():
        shutil.copyfile(stock, vendor)
        for path in [calls, menu/'loaded-boot', base/'backend-down', base/'input-rejected']:
            path.unlink(missing_ok=True)
        if (base/'attempted').exists():
            (base/'attempted').rmdir()
    def run(expected):
        result = subprocess.run(['sh', str(script)], env=env, timeout=15)
        assert result.returncode == expected, result.returncode
        # Production intentionally detaches dt run. Let this synchronous mock
        # finish recording before inspecting or removing its temporary paths.
        time.sleep(.05)
        return calls.read_text().splitlines() if calls.exists() else []
    reset()
    log = run(0)
    assert log.count('dt stop') == log.count('dt run') == 1, log
    assert 'ui --check-api --idle' in log and 'ui --probe-input' in log and 'supervisor' in log, log
    assert vendor.read_bytes() == policy.read_bytes()
    calls.unlink()
    log = run(0)
    assert log == ['supervisor'], log
    print('PASS one cold-boot reload, API idle gate, MENU probe, cached supervisor rejoin')
    reset()
    (base/'backend-down').touch()
    log = run(1)
    assert not any(line.startswith(('dt ', 'mount', 'supervisor')) for line in log), log
    assert vendor.read_bytes() == stock.read_bytes()
    calls.unlink()
    assert run(1) == []
    print('PASS unavailable backend retains stock, no repeated boot reload')
    reset()
    (base/'input-rejected').touch()
    log = run(1)
    assert log.count('dt stop') == log.count('dt run') == 2, log
    assert log.count('umount') == 1 and 'supervisor' not in log
    assert vendor.read_bytes() == stock.read_bytes()
    print('PASS failed MENU routing restores stock with one bounded recovery reload')
    reset()
    vendor.write_bytes(b'unknown firmware')
    log = run(1)
    assert not any(line.startswith(('dt ', 'mount', 'supervisor')) for line in log), log
    print('PASS unknown firmware fails before modifying the stock map')

# Production supervisor: exactly three crash restarts, then stock recovery.
with tempfile.TemporaryDirectory(prefix='hr54-native-supervisor-') as td:
    base=Path(td); menu=base/'menu'; menu.mkdir(); commands=base/'commands'; commands.mkdir()
    calls=base/'calls'; lock=base/'lock'
    for flag in ['ENABLED','ACTIVATED']: (menu/flag).touch()
    def executable(path, body):
        path.write_text('#!/bin/sh\nset -eu\n'+body+'\n'); path.chmod(0o700)
    executable(commands/'pidof','echo 1')
    executable(commands/'sleep','/bin/sleep .01')
    executable(menu/'hr54-ui',"if [ \"${1:-}\" = --check-api ]; then exit 0; fi; echo start >> \"$SUP_CALLS\"; exit 1")
    supervisor=base/'supervisor.sh'
    supervisor.write_text((root/'supervisor.sh').read_text().replace('/var/hr54-persist/native-menu',str(menu)).replace('/tmp/hr54-ui-supervisor.lock',str(lock)))
    env=dict(os.environ,PATH=str(commands)+':'+os.environ['PATH'],SUP_CALLS=str(calls))
    subprocess.run(['sh',str(supervisor)],env=env,check=True,timeout=10)
    assert calls.read_text().splitlines()==['start']*3
    assert not lock.exists() and not (menu/'ui.pid').exists()
    assert 'restart budget exhausted' in (menu/'startup.log').read_text()
    print('PASS production supervisor crash budget and PID/lock cleanup')

# The real typed health-check CLI fails closed on old/malformed readiness.
class Readiness(http.server.BaseHTTPRequestHandler):
    state={'ready':True,'frontend':'native','doomRunning':False,'mediaBusy':False}
    def log_message(self,*args): pass
    def do_GET(self):
        raw=json.dumps(self.state).encode(); self.send_response(200); self.send_header('Content-Length',str(len(raw))); self.end_headers(); self.wfile.write(raw)
with http.server.ThreadingHTTPServer(('127.0.0.1',0),Readiness) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    def check(expected,idle=False):
        args=[str(root/'build/hr54-ui-host'),'--port',str(server.server_port),'--check-api']+(['--idle'] if idle else [])
        assert subprocess.run(args,timeout=8).returncode==expected
    check(0); check(0,True)
    Readiness.state['mediaBusy']=True; check(0); check(1,True)
    del Readiness.state['mediaBusy']; check(0); check(1,True)
    Readiness.state['frontend']='legacy'; check(1)
    Readiness.state={}; check(1)
    server.shutdown()
print('PASS typed API readiness, media-idle gate and malformed/legacy refusal')
