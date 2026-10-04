#!/usr/bin/env python3
"""Independent vendor peer + persistent broker + changing consumers."""
from pathlib import Path
import socket,struct,subprocess,threading,time
root=Path(__file__).resolve().parents[1]
subprocess.run(['cc','-std=c11','-O1','-Wno-misleading-indentation','-I.','-I../tools/native-ui','-o','build/hr54-input-broker-host','input/broker.c','input/dispatcher.c','input/keys.c'],cwd=root,check=True)
def pkt(*w):return struct.pack('>'+str(len(w))+'I',*w)
def readn(s,n):
 d=b''
 while len(d)<n:
  p=s.recv(n-len(d));assert p;d+=p
 return d
def frame(s):
 n=struct.unpack('>I',readn(s,4))[0];return (n,)+struct.unpack('>'+str(n//4-1)+'I',readn(s,n-4))
events=[];ready=threading.Event();send_event=threading.Event();errors=[]
with socket.socket() as hw:
 hw.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);hw.bind(('127.0.0.1',47952));hw.listen(1)
 def vendor():
  try:
   with hw.accept()[0] as c:
    c.sendall(pkt(77));assert frame(c)==(12,16,0);m=frame(c);assert m[1:4]==(0,77,1);assert len(m[6:])==43 and len(set(m[6:]))==43
    c.sendall(pkt(12,7,1)[:5]);time.sleep(.01);c.sendall(pkt(12,7,1)[5:]);ready.set();send_event.wait(3)
    # 28-byte press, 16-byte repeat, 24-byte release: dispatcher must retain fragments.
    for p in (pkt(28,8,0x1e103,0,0,1,1),pkt(16,8,0x1e103,0),pkt(24,8,0xe103,0,0,1)):
     c.sendall(p[:3]);time.sleep(.005);c.sendall(p[3:])
    time.sleep(.2);c.settimeout(.15)
    try: extra=c.recv(1)
    except socket.timeout: extra=b''
    assert extra==b'',f'hardware map changed after consumer handoff: {extra!r}'
  except BaseException as e:errors.append(e)
 t=threading.Thread(target=vendor);t.start();broker=subprocess.Popen([str(root/'build/hr54-input-broker-host')],stderr=subprocess.PIPE)
 try:
  assert ready.wait(3)
  def consumer(keys):
   s=socket.create_connection(('127.0.0.1',47953),timeout=2);owner=struct.unpack('>I',readn(s,4))[0];s.sendall(pkt(12,16,0));s.sendall(pkt(24+4*len(keys),0,owner,1,1,len(keys)+1,*keys));assert frame(s)==(12,7,1);return s,owner
  ui,uo=consumer([0xe503,0xe00b]);game,go=consumer([0xe103,0xe402]);
  # Changing the UI consumer map cannot touch the retained vendor registration.
  ui.sendall(pkt(28,0,uo,1,1,2,0xe503));assert frame(ui)==(12,7,1)
  send_event.set();game.settimeout(2)
  got=[frame(game) for _ in range(3)];assert [x[2] for x in got]==[0x1e103,0x1e103,0xe103],got
  ui.settimeout(.2)
  try: leaked=ui.recv(1)
  except socket.timeout: leaked=b''
  assert leaked==b'',leaked
  ui.close();game.close();t.join(3);assert not errors,errors
 finally:
  broker.terminate();broker.wait(timeout=3)
print('PASS persistent hardware map, fragmented 16/24/28-byte events, repeat/release, consumer handoff and no duplicate routing')
