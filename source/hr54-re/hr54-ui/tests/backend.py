#!/usr/bin/env python3
"""Test native API additions against a real isolated backend and upstream mock."""
from pathlib import Path
import subprocess,tempfile,os,socket,time,json,urllib.request,urllib.error,signal,http.server,threading
root=Path(__file__).resolve().parents[2];backend=root/'jellyfin/remote';calls=[]
class Upstream(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  calls.append(self.path)
  if '/Images/Primary' in self.path:
   raw=b'ART';self.send_response(200);self.send_header('Content-Type','image/jpeg');self.send_header('Content-Length','3');self.end_headers();self.wfile.write(raw);return
  data={'Id':'user','Name':'viewer'} if self.path=='/Users/Me' else {'Items':[{'Id':'movie1','Name':'A film','Type':'Movie','MediaType':'Video','RunTimeTicks':1200000000,'UserData':{'PlaybackPositionTicks':300000000}}],'TotalRecordCount':1}
  raw=json.dumps(data).encode();self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
 def do_POST(self):
  self.rfile.read(int(self.headers.get('Content-Length','0')));self.send_response(200);self.end_headers();self.wfile.write(b'{}')
with http.server.ThreadingHTTPServer(('127.0.0.1',0),Upstream) as upstream,tempfile.TemporaryDirectory(prefix='native-api-') as td:
 threading.Thread(target=upstream.serve_forever,daemon=True).start();t=Path(td);persist=t/'persist';(persist/'config').mkdir(parents=True);(persist/'doom/bin').mkdir(parents=True);(persist/'doom/data').mkdir()
 # Isolate the existing stock-control adapter and record its fixed XML.
 stop_mock=t/'uconntest';stop_args=t/'stop-args';stop_mock.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$HR54_STOP_MOCK_ARGS"\nprintf "%s\\n" "$HR54_STOP_MOCK_REPLY"\nexit 7\n');stop_mock.chmod(0o700)
 stop_test=t/'stop-test';subprocess.run(['cc','-O1',f'-DJF_UCONNECT_BIN="{stop_mock}"','-o',str(stop_test),str(root/'hr54-ui/tests/test_backend_stop.c')],check=True)
 subprocess.run([str(stop_test)],env=dict(os.environ,HR54_STOP_MOCK_ARGS=str(stop_args)),check=True)
 assert stop_args.read_text().splitlines()==['<com.ucentric.pvruconnect.DirectTest command="watch" session="0" speed="stop"/>']*3
 (persist/'iptv').mkdir();(persist/'iptv/eng.m3u').write_text('#EXTM3U\n'+''.join(f'#EXTINF:-1 group-title="Group {i}",Channel {i}\nhttp://example.invalid/{i}.ts\n' for i in range(65)))
 # Token never leaves this isolated mock. Match the backend's persistent schema.
 (persist/'config/config.json').write_text(json.dumps({'userId':'user','username':'viewer','token':'TEST_TOKEN'}));(persist/'config/token').write_text('TEST_TOKEN')
 osd=t/'osd-number';prepare_mock=t/'prepare-uconntest';prepare_args=t/'prepare-args'
 prepare_mock.write_text("""#!/bin/sh
printf '%s\n' "$*" >> "$HR54_PREPARE_ARGS"
if [ ! -f "$HR54_PREPARE_FAIL" ]; then printf '"0"' > "$HR54_PREPARE_OSD"; fi
printf '%s\n' 'OsdManager response'
exit 0
""");prepare_mock.chmod(0o700)
 exe=t/'backend';subprocess.run(['cc','-O1',f'-DJF_BOOT_OSD_NUMBER="{osd}"',f'-DJF_DT_BIN="{prepare_mock}"','-o',str(exe),str(backend/'hr54_jf.c')],check=True)
 sock=socket.socket();sock.bind(('127.0.0.1',0));port=sock.getsockname()[1];sock.close();env=dict(os.environ,JF_PERSIST_ROOT=str(persist),HR54_PREPARE_ARGS=str(prepare_args),HR54_PREPARE_OSD=str(osd),HR54_PREPARE_FAIL=str(t/'prepare-fail'));p=subprocess.Popen([str(exe),str(backend/'static'),'127.0.0.1',str(upstream.server_port),str(port),'--native-frontend'],env=env,start_new_session=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 def api(path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
  try:
   with urllib.request.urlopen(req,timeout=12) as r:return r.status,json.load(r)
  except urllib.error.HTTPError as e:return e.code,json.load(e)
 try:
  time.sleep(.3);assert api('/api/system/status')[1]['frontend']=='native';assert api('/api/capabilities')[1]['doom'];assert not api('/api/doom/status')[1]['running'];assert api('/api/doom/start',{})[0]==502
  assert api('/api/system/frontend/prepare',{'osd':1})[0]==400
  assert api('/api/system/frontend/prepare',{})[1]=={'ok':True,'prepared':True,'dismissedBootAlert':False};assert not prepare_args.exists()
  osd.write_text('"36"');assert api('/api/system/frontend/prepare',{})[1]=={'ok':True,'prepared':True,'dismissedBootAlert':True}
  assert prepare_args.read_text().splitlines()==['removeOsd -osd 36 -session 0']
  osd.write_text('"37"');assert not api('/api/system/frontend/prepare',{})[1]['dismissedBootAlert'];assert len(prepare_args.read_text().splitlines())==1
  osd.write_text('"36"');(t/'prepare-fail').touch();assert api('/api/system/frontend/prepare',{})[0]==502;assert osd.read_text()=='"36"';(t/'prepare-fail').unlink();osd.write_text('"0"')
  assert api('/api/tv/exit',{})[1]['stopped'];assert api('/api/doom/stop',{})[1]['running'] is False
  groups=api('/api/iptv/groups?limit=59&offset=0')[1];assert len(groups['groups'])==59 and groups['groupCount']==65 and groups['total']==65 and groups['hasMore'];assert api('/api/iptv/groups?limit=60&offset=59')[1]['groups'][0]['name']=='Group 59';assert len(api('/api/iptv/groups')[1]['groups'])==65
  code,resume=api('/api/jellyfin/resume');assert code==200,(code,resume);assert resume['items'][0]['resumeSeconds']==30;assert any('Filters=IsResumable' in path for path in calls)
  with urllib.request.urlopen(f'http://127.0.0.1:{port}/art/native/movie1.jpg') as r:assert r.read()==b'ART'
  assert any('maxWidth=512' in path and 'maxHeight=512' in path for path in calls)
  (persist/'doom/ENABLED').touch();(persist/'doom/data/doom1.wad').write_bytes(b'TEST')
  source=t/'engine.c';source.write_text('#include <signal.h>\n#include <unistd.h>\n#include <stdlib.h>\n#include <string.h>\nint main(int argc,char **argv){for(int i=1;i+1<argc;i++)if(!strcmp(argv[i],"--ready-fd")){int fd=atoi(argv[i+1]);char r=1;write(fd,&r,1);close(fd);}for(;;)pause();}\n');subprocess.run(['cc','-o',str(persist/'doom/bin/hr54-doom-native'),str(source)],check=True)
  code,data=api('/api/doom/start',{});assert code==200 and data['running'],(code,data);engine=data['pid']
  args=Path(f'/proc/{engine}/cmdline').read_bytes().split(b'\0');assert args[args.index(b'--depth')+1]==b'2000',args;assert args[args.index(b'--seconds')+1]==b'0',args
  assert api('/api/doom/start',{})[1]['running'];assert api('/api/doom/status')[1]['running'];assert api('/api/system/status')[1]['doomRunning'];code,stop=api('/api/doom/stop',{});assert code==200 and stop['running'] is False,(code,stop);assert not api('/api/doom/status')[1]['running']
  # A stale PID naming a different executable must never be signalled.
  (persist/'doom/doom.pid').write_text(str(p.pid));assert not api('/api/doom/status')[1]['running'];assert api('/api/doom/stop',{})[0]==200;assert p.poll() is None
  print('PASS native readiness/mode, no ITV exit dependency, Continue Watching/resume model, sized artwork/cache, Doom gating/start/status/idempotency/clean stop/PID identity')
 finally:
  os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
 upstream.shutdown()
