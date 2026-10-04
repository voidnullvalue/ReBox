#!/usr/bin/env python3
"""Offline keydispatcher codec. No sockets, receiver execution or deployment."""
import argparse
import json
import struct

ENDPOINT = ('127.0.0.1', 47952)  # receiver-local service, documentation only
PRESS_MASK = 0x10000
KEY_MASK = 0xffff
MAX_FRAME = 4096  # host decoder policy, not a recovered server limit


def words(*values):
    if any(not isinstance(v,int) or not 0<=v<=0xffffffff for v in values):
        raise ValueError('word outside uint32')
    return struct.pack('>'+str(len(values))+'I',*values)


def frame(command,*payload):
    return words(8+4*len(payload),command,*payload)


def session(session_id=0):return frame(16,session_id)
def close(owner):return frame(4,owner)
def push(owner,raw):return frame(5,owner,raw)


def register(owner,device=1,shared=(),exclusive=()):
    """Replace this owner's map. Wire counts include an unsent zero sentinel."""
    shared,exclusive=tuple(shared),tuple(exclusive)
    if any(not isinstance(k,int) or not 0<k<=KEY_MASK for k in shared+exclusive):
        raise ValueError('registration requires nonzero 16-bit key codes')
    if len(set(shared+exclusive))!=len(shared+exclusive):raise ValueError('duplicate key in map')
    return frame(0,owner,device,len(shared)+1,*shared,len(exclusive)+1,*exclusive)


def decode(data):
    if len(data)<8 or len(data)%4 or len(data)>MAX_FRAME:raise ValueError('invalid frame bounds')
    v=struct.unpack('>'+str(len(data)//4)+'I',data)
    if v[0]!=len(data):raise ValueError('length mismatch')
    result=dict(length=v[0],command=v[1],words=list(v[2:]))
    if v[1]==0:
        if len(v)<6:raise ValueError('short map')
        owner,device,count=v[2:5]
        if count<1 or 4+count>=len(v):raise ValueError('invalid shared count')
        split=4+count;exclusive_count=v[split]
        if exclusive_count<1 or split+exclusive_count!=len(v):raise ValueError('invalid exclusive count')
        result.update(owner=owner,device=device,shared=list(v[5:split]),exclusive=list(v[split+1:]))
    elif v[1]==7:
        if len(v)<3 or v[2]<1 or v[2]!=len(v)-2:raise ValueError('invalid map acknowledgement')
        result.update(accepted=v[2]==1,rejected=list(v[3:]))
    elif v[1]==8:
        if len(data) not in (12,16,24,28):raise ValueError('unrecognized input event layout')
        raw=v[2];result.update(raw=raw,key=raw&KEY_MASK,pressed=bool(raw&PRESS_MASK))
        if len(v)>3:result['session']=v[3]
        if len(v)>=6:result['timestamp']=(v[4]<<32)|v[5]
        if len(v)==7:result['device']=v[6]
    return result


class StreamDecoder:
    """Feed captured chunks after the separate four-byte greeting has been read."""
    def __init__(self):self.pending=bytearray()
    def feed(self,chunk):
        self.pending.extend(chunk);out=[]
        while len(self.pending)>=4:
            n=int.from_bytes(self.pending[:4],'big')
            if n<8 or n>MAX_FRAME or n%4:self.pending.clear();raise ValueError('invalid stream length')
            if len(self.pending)<n:break
            packet=bytes(self.pending[:n]);del self.pending[:n];out.append(decode(packet))
        return out


class KeyTracker:
    """Preserve every event; identify repeated press until matching release.

    This is a host policy, not a vendor debounce or a fabricated repeat bit.
    """
    def __init__(self):self.down=set()
    def consume(self,event):
        if event['command']!=8:raise ValueError('not an input event')
        key=(event.get('session'),event.get('device'),event['key'])
        repeated=event['pressed'] and key in self.down
        if event['pressed']:self.down.add(key)
        else:self.down.discard(key)
        return dict(event,repeated=repeated)


def main():
    p=argparse.ArgumentParser(description=__doc__);s=p.add_subparsers(dest='mode',required=True)
    e=s.add_parser('encode');e.add_argument('operation',choices=['session','register','close','push']);e.add_argument('--owner',type=lambda x:int(x,0),default=0)
    e.add_argument('--session',type=int,default=0);e.add_argument('--device',type=int,default=1)
    e.add_argument('--shared',type=lambda x:int(x,0),nargs='*',default=[]);e.add_argument('--exclusive',type=lambda x:int(x,0),nargs='*',default=[]);e.add_argument('--raw',type=lambda x:int(x,0),default=0)
    d=s.add_parser('decode');d.add_argument('hex')
    a=p.parse_args()
    if a.mode=='decode':print(json.dumps(decode(bytes.fromhex(a.hex)),indent=2))
    else:
        out={'session':lambda:session(a.session),'register':lambda:register(a.owner,a.device,a.shared,a.exclusive),'close':lambda:close(a.owner),'push':lambda:push(a.owner,a.raw)}[a.operation]()
        print(out.hex())


if __name__=='__main__':main()
