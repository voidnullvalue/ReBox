#!/usr/bin/env python3
"""Offline BinaryIPC framing and recovered primitive MediaPlayer messages."""
import argparse
import json
import struct
from pathlib import Path

SCHEMA_FILE=Path(__file__).resolve().parents[2]/'docs/native-evidence/media-schemas.json'
MAX_FRAME=1024*1024  # host policy


def word(v):
    if not isinstance(v,int) or not 0<=v<=0xffffffff:raise ValueError('word outside uint32')
    return struct.pack('>I',v)


def encode(tag,sequence=0,fields=()):
    body=b''.join(word(8+len(value))+word(field_tag)+value for field_tag,value in fields)
    if len(body)+12>MAX_FRAME:raise ValueError('message too large')
    return word(12+len(body))+word(tag)+word(sequence)+body


def decode(packet):
    if len(packet)<12 or len(packet)>MAX_FRAME:raise ValueError('invalid message bounds')
    length,tag,sequence=struct.unpack_from('>3I',packet)
    if length!=len(packet):raise ValueError('message length mismatch')
    fields=[];pos=12
    while pos<len(packet):
        if pos+8>len(packet):raise ValueError('truncated field header')
        n,t=struct.unpack_from('>2I',packet,pos)
        if n<8 or pos+n>len(packet):raise ValueError('invalid field length')
        fields.append((t,packet[pos+8:pos+n]));pos+=n
    return dict(tag=tag,sequence=sequence,fields=fields)


def schema(name):
    return next(s for s in json.loads(SCHEMA_FILE.read_text())['schemas'] if s['name']==name)


def primitive(descriptor,value):
    formats={'(B)V':('b',8),'(S)V':('h',16),'(I)V':('i',32),'(J)V':('q',64)}
    if descriptor=='(Z)V':
        if value not in (0,1,False,True):raise ValueError('boolean must be 0 or 1')
        return bytes([int(value)])
    if descriptor not in formats:raise ValueError('schema requires unrecovered non-primitive serialization')
    fmt,bits=formats[descriptor]
    if not isinstance(value,int) or not -(1<<(bits-1))<=value<(1<<(bits-1)):raise ValueError('signed primitive outside range')
    return struct.pack('>'+fmt,value)


def message(name,sequence=0,**values):
    s=schema(name);known={f['name']:f for f in s['fields']}
    if not s['message_tag']:raise ValueError('message constructor tag unresolved')
    if set(values)-known.keys():raise ValueError('unknown schema fields')
    fields=[]
    for name,value in values.items():
        f=known[name]
        if f['tag'] is None:raise ValueError('field tag unresolved')
        fields.append((int(f['tag'],0),primitive(f['descriptor'],value)))
    return encode(int(s['message_tag'],0),sequence,fields)


def main():
    p=argparse.ArgumentParser(description=__doc__);s=p.add_subparsers(dest='mode',required=True)
    e=s.add_parser('encode');e.add_argument('name');e.add_argument('--sequence',type=int,default=0);e.add_argument('--field',action='append',default=[])
    d=s.add_parser('decode');d.add_argument('hex');s.add_parser('dump')
    a=p.parse_args()
    if a.mode=='dump':print(SCHEMA_FILE.read_text())
    elif a.mode=='decode':print(json.dumps(decode(bytes.fromhex(a.hex)),default=lambda b:b.hex(),indent=2))
    else:
        values={}
        for item in a.field:
            key,val=item.split('=',1)
            if key in values:raise ValueError('duplicate named field')
            values[key]=int(val,0)
        print(message(a.name,a.sequence,**values).hex())


if __name__=='__main__':main()
