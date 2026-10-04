#!/usr/bin/env python3
"""Run the real rollback against isolated files and owned dummy processes."""
from pathlib import Path
import tempfile,subprocess,os,time,hashlib
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hr54-native-rollback-') as td:
 base=Path(td);menu=base/'native-menu';backend=base/'jellyfin';commands=base/'commands';calls=base/'calls'
 for p in [menu/'backup',backend/'bin',backend/'doom/bin',backend/'doom/data',backend/'ui',commands]:p.mkdir(parents=True,exist_ok=True)
 source=base/'dummy.c';source.write_text('#include <unistd.h>\n#include <stdlib.h>\nint main(int n,char **v){(void)v;if(n>1)return getenv("ROLLBACK_BUSY")?1:0;for(;;)pause();}\n')
 subprocess.run(['cc','-O1','-o',str(menu/'hr54-ui'),str(source)],check=True)
 original=(menu/'hr54-ui').read_bytes();(menu/'backup/hr54-jf').write_bytes(original);(menu/'backup/hr54-jf').chmod(0o700)
 (backend/'bin/hr54-jf').write_bytes(original);(backend/'bin/hr54-jf').chmod(0o700)
 for flag in ['ENABLED','ACTIVATED']:(menu/flag).touch()
 (backend/'doom/ENABLED').touch();(menu/'backup/doom-DISABLED').touch();(backend/'ui/hijack-pause').touch()
 (backend/'doom/bin/hr54-doom-native').write_bytes(b'new engine')
 for name in ['doom1.wad','README.TXT','SOURCE.md']:(backend/'doom/data'/name).write_text('new');(menu/'backup'/name).write_text('original')
 for name in ['dt','umount']:
  p=commands/name;p.write_text('#!/bin/sh\nprintf "%s %s\\n" "'+name+'" "$*" >> "$ROLLBACK_CALLS"\n');p.chmod(0o700)
 script=base/'rollback.sh';script.write_text((root/'tools/rollback-receiver.sh').read_text().replace('/var/hr54-persist/native-menu',str(menu)).replace('/var/hr54-persist/jellyfin',str(backend)).replace('/tmp/hr54-ui-supervisor.lock',str(base/'supervisor-lock')))
 ui=subprocess.Popen([str(menu/'hr54-ui')]);api=subprocess.Popen([str(backend/'bin/hr54-jf')]);(menu/'ui.pid').write_text(str(ui.pid));(backend/'jf.pid').write_text(str(api.pid))
 env=dict(os.environ,PATH=str(commands)+':'+os.environ['PATH'],ROLLBACK_CALLS=str(calls))
 try:
  result=subprocess.run(['sh',str(script)],env=dict(env,ROLLBACK_BUSY='1'),timeout=5,capture_output=True)
  assert result.returncode==1 and (menu/'ACTIVATED').exists() and ui.poll() is None and api.poll() is None
  assert not calls.exists()
  subprocess.run(['sh',str(script)],env=env,check=True,timeout=8,capture_output=True)
  ui.wait(timeout=3);api.wait(timeout=3);time.sleep(.05)
  assert not (menu/'ENABLED').exists() and not (menu/'ACTIVATED').exists()
  assert (backend/'bin/hr54-jf').read_bytes()==original
  assert (backend/'doom/DISABLED').exists() and not (backend/'doom/ENABLED').exists()
  assert not (backend/'doom/bin/hr54-doom-native').exists() and not (backend/'ui/hijack-pause').exists()
  for name in ['doom1.wad','README.TXT','SOURCE.md']:assert (backend/'doom/data'/name).read_text()=='original'
  log=calls.read_text().splitlines();assert log.count('dt stop')==1 and log.count('dt run')==1 and 'umount /opt/dtv/dtv.car' in log,log
 finally:
  for p in [ui,api]:
   if p.poll() is None:p.terminate();p.wait(timeout=3)
print('PASS production rollback media gate, identity-checked shutdown, backend/assets/markers restore and one stock reload')
