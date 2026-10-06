#!/usr/bin/env python3
"""Host integration tests, independent of the receiver and external services."""
from pathlib import Path
import subprocess,http.server,socketserver,threading,time,json,tempfile,os
from PIL import Image
root=Path(__file__).resolve().parents[1]
if os.environ.get('HR54_SANITIZE')=='1':
 os.environ['ASAN_OPTIONS']='detect_leaks=1'
core=['api/json.c','api/client.c','api/models.c','input/keys.c','input/dispatcher.c','apps/controller.c','apps/fixtures.c','ui/animation.c','ui/draw.c','ui/image.c','ui/text.c','ui/artwork.c','../tools/native-ui/framebuffer.c']
core += [str(p.relative_to(root)) for p in sorted((root/'api').glob('*.c')) if p.name not in ('json.c','client.c','models.c')]
flags=['cc','-std=c11','-O1','-g','-I.','-I../tools/native-ui','-fno-omit-frame-pointer']
if os.environ.get('HR54_SANITIZE')=='1':flags+=['-fsanitize=address,undefined']
subprocess.run(flags+['-o','build/test-ui','tests/test_core.c']+core+['-lm','-pthread'],cwd=root,check=True)
subprocess.run([str(root/'build/test-ui')],check=True)
Image.new('RGBA',(512,512),(90,130,170,255)).save(root/'build/test-artwork.png')
subprocess.run(flags+['-o','build/test-artwork','tests/test_artwork.c','ui/artwork.c','ui/draw.c','ui/image.c','ui/text.c','../tools/native-ui/framebuffer.c','-lm','-pthread'],cwd=root,check=True)
subprocess.run([str(root/'build/test-artwork'),str(root/'build/test-artwork.png')],check=True)
subprocess.run(flags+['-o','build/test-client','tests/test_client.c','api/json.c','api/client.c'],cwd=root,check=True)
class Wire(socketserver.BaseRequestHandler):
 def handle(self):
  data=b''
  while b'\r\n\r\n' not in data:
   part=self.request.recv(4096)
   if not part:return
   data+=part
  path=data.split(b' ')[1]
  if path==b'/slow':time.sleep(.3);return
  if path==b'/huge-header':self.request.sendall(b'HTTP/1.0 200 OK\r\nX: '+b'x'*9000);return
  raw=b'{"ok":true}'
  length=len(raw)+10 if path==b'/short' else len(raw)
  packet=f'HTTP/1.0 200 OK\r\nContent-Length: {length}\r\n\r\n'.encode()+raw
  for i in range(0,len(packet),3):self.request.sendall(packet[i:i+3]);time.sleep(.001)
  if path==b'/keep-open':time.sleep(.4)
class Threads(socketserver.ThreadingTCPServer):allow_reuse_address=True;daemon_threads=True
with Threads(('127.0.0.1',0),Wire) as server:
 threading.Thread(target=server.serve_forever,daemon=True).start()
 for path in ['/ok','/keep-open','/slow','/short','/huge-header']:
  subprocess.run([str(root/'build/test-client'),str(server.server_address[1]),path],check=True)
 server.shutdown()
browse_started=threading.Event();playback_active=threading.Event();playback_seen=threading.Event();posted=[]
class Backend(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  posted.append(self.path);self.rfile.read(int(self.headers.get("Content-Length","0")));raw=b'{"prepared":true,"dismissedBootAlert":false}';self.send_response(200);self.send_header("Content-Length",str(len(raw)));self.end_headers();self.wfile.write(raw)
 def do_GET(self):
  path=self.path
  if path.startswith('/api/modules/example-0/browse'):
   browse_started.set();time.sleep(1.2);data={'items':[{'id':'late','title':'Late response','kind':'item'}], 'total':1,'offset':0,'hasMore':False}
  elif path=='/api/modules':data={'moduleApi':1,'modules':[{'id':f'example-{i}','name':f'Module {i+1}','version':'1.0','description':'Independent receiver module','installed':True,'enabled':True,'healthy':True,'capabilities':{'browse':True,'search':True,'playback':True}} for i in range(5)]}
  elif path=='/api/system/status':data={'ready':True,'frontend':'native','nativeModule':'','mediaBusy':False}
  elif path=='/api/auth/status':data={'authenticated':True}
  else:
   playing=playback_active.is_set();data={'playing':playing,'elapsed':0,'title':'Movie' if playing else '', 'source':'example-0' if playing else '', 'generation':1 if playing or playback_seen.is_set() else 0,'transport':{}}
   if playing:playback_seen.set()
  raw=json.dumps(data).encode()
  try:self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
  except BrokenPipeError:pass
with http.server.ThreadingHTTPServer(('127.0.0.1',0),Backend) as server,tempfile.TemporaryDirectory() as td:
 threading.Thread(target=server.serve_forever,daemon=True).start()
 output=Path(td)/'screen.rgba'
 reference=Path(td)/'home.rgba';subprocess.run([str(root/'build/hr54-ui-host'),'--preview','home-five','--output',str(reference)],check=True);home=reference.read_bytes()
 def wait_home(seconds):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   if output.exists() and output.read_bytes()==home:return
   time.sleep(.01)
  raise AssertionError('native home did not appear before deadline')
 p=subprocess.Popen([str(root/'build/hr54-ui-host'),'--port',str(server.server_port),'--still','--seconds','5','--output',str(output)],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL)
 try:
  wait_home(3);p.stdin.write(b' ');p.stdin.flush();assert browse_started.wait(2),'browse request not started'
  p.stdin.write(b'b');p.stdin.flush();wait_home(.5)
  time.sleep(1.4);assert output.read_bytes()==home,'stale response replaced home'
  p.wait(timeout=6);assert p.returncode==0
 finally:
  if p.poll() is None:p.terminate();p.wait(timeout=5)
 # A presentation-only update must begin transparent without media operations.
 before=len(posted)
 p=subprocess.Popen([str(root/'build/hr54-ui-host'),'--port',str(server.server_port),'--still','--start-hidden','--seconds','4','--output',str(output)],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL)
 try:
  end=time.monotonic()+2
  while time.monotonic()<end:
   if output.exists() and output.read_bytes()==bytes(720*480*4):break
   time.sleep(.01)
  else:raise AssertionError('start-hidden did not publish transparency')
  assert len(posted)==before,'hidden startup issued an application/device operation'
  p.stdin.write(b'g');p.stdin.flush();wait_home(1)
  time.sleep(.1);assert posted[before:]==['/api/system/frontend/prepare'],posted[before:]
  p.terminate();p.wait(timeout=5)
 finally:
  if p.poll() is None:p.terminate();p.wait(timeout=5)
 # Playback ending after a hidden restart must display Home, without a key
 # press or media command, and prepare navigation through the backend API.
 before=len(posted);playback_active.set();playback_seen.clear();output.unlink()
 p=subprocess.Popen([str(root/'build/hr54-ui-host'),'--port',str(server.server_port),'--still','--start-hidden','--seconds','5','--output',str(output)],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL)
 try:
  assert playback_seen.wait(2),'hidden startup did not observe existing playback'
  assert output.exists() and output.read_bytes()==bytes(720*480*4),'playing source lost transparency'
  assert len(posted)==before,'hidden startup changed media/device state'
  playback_active.clear();wait_home(2)
  end=time.monotonic()+1
  while len(posted)==before and time.monotonic()<end:time.sleep(.01)
  assert posted[before:]==['/api/system/frontend/prepare'],posted[before:]
  p.terminate();p.wait(timeout=5)
 finally:
  if p.poll() is None:p.terminate();p.wait(timeout=5)
 server.shutdown()
print('PASS real reactor readiness, asynchronous browse, BACK latency, stale replies, hidden playback-end restoration')
subprocess.run(['python3',str(root/'tests/audit.py')],check=True)
subprocess.run(['python3',str(root/'tests/backend.py')],check=True)

subprocess.run(["python3",str(root/"tests/boot.py")],check=True)

subprocess.run(["python3",str(root/"tests/activation.py")],check=True)

subprocess.run(["python3",str(root/"tests/input.py")],check=True)

subprocess.run(["python3",str(root/"tests/rollback.py")],check=True)
