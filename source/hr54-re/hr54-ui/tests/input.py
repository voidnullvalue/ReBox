#!/usr/bin/env python3
"""Real dispatcher against an independent wire peer, including fragmented events."""
from pathlib import Path
import socket,struct,threading,subprocess,time
root=Path(__file__).resolve().parents[1]
subprocess.run(['cc','-std=c11','-O1','-I.','-o','build/test-input','tests/test_input.c','input/dispatcher.c','input/keys.c'],cwd=root,check=True)
errors=[]
def packet(*words): return struct.pack('>'+str(len(words))+'I',*words)
with socket.socket() as server:
 server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);server.bind(('127.0.0.1',47952));server.listen(1);server.settimeout(5)
 def peer():
  try:
   with server.accept()[0] as conn:
    conn.settimeout(5)
    def read(n):
     data=b''
     while len(data)<n:
      chunk=conn.recv(n-len(data));assert chunk;data+=chunk
     return data
    conn.sendall(packet(23));assert read(12)==packet(12,16,0)
    for expected in [0,39,8,39,0,39]:
     n=struct.unpack('>I',read(4))[0];words=struct.unpack('>'+str(n//4-1)+'I',read(n-4))
     assert words[:3]==(0,23,1),words
     shared_count=words[3]-1;assert shared_count==0,words
     exclusive_count=words[4]-1;exclusive=words[5:]
     assert exclusive_count==expected+1 and exclusive[0]==0xe503,words
     assert len(set(exclusive))==exclusive_count,words
     if expected==8:assert 0xe00e in exclusive and 0xe100 not in exclusive
     if expected==39:assert 0xe100 in exclusive and 0xe001 in exclusive
     # The daemon sends type-7 ownership-loss notices during selective
     # release as well as an empty successful map acknowledgement.
     if expected in (0,8):conn.sendall(packet(20,7,3,0xffffe100,0xffffe001))
     ack=packet(12,7,1);conn.sendall(ack[:3]);time.sleep(.005);conn.sendall(ack[3:])
    for raw in [0x1e103,0x1e103,0xe103]:
     event=packet(28,8,raw,0,0,100,1);conn.sendall(event[:7]);time.sleep(.005);conn.sendall(event[7:])
    assert read(12)==packet(12,4,23)
  except BaseException as e: errors.append(e)
 thread=threading.Thread(target=peer);thread.start()
 result=subprocess.run([str(root/'build/test-input')],timeout=6);thread.join(6)
 assert not errors,errors
 assert result.returncode==0
for denied_key in (0xe503,0xe103,0xe001,0xe400):
 with socket.socket() as server:
  server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);server.bind(('127.0.0.1',47952));server.listen(1);server.settimeout(5)
  def denied():
   try:
    with server.accept()[0] as conn:
     conn.settimeout(5)
     def read(n):
      data=b''
      while len(data)<n:
       part=conn.recv(n-len(data));assert part;data+=part
      return data
     conn.sendall(packet(23));assert read(12)==packet(12,16,0)
     n=struct.unpack('>I',read(4))[0];read(n-4)
     # Navigation denial is checked after a real full-map request.
     conn.sendall(packet(12,7,1))
     n=struct.unpack('>I',read(4))[0];read(n-4)
     conn.sendall(packet(16,7,2,0xffff0000|denied_key));assert read(12)==packet(12,4,23)
   except BaseException as e: errors.append(e)
  thread=threading.Thread(target=denied);thread.start()
  result=subprocess.run([str(root/'build/test-input'),'denied'],timeout=6);thread.join(6)
  assert not errors,errors
  assert result.returncode==0
print('PASS denied MENU, arrows, SELECT and transport fail closed and releases the owned session')
print('PASS production input wire handoff, exclusive navigation and MENU, partial ACK/event, press/release/repeat and cleanup')
