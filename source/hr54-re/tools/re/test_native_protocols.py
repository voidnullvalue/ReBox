#!/usr/bin/env python3
"""Offline golden packets, parser rejection, streaming and repeat tests."""
import importlib.util
import struct
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]


def load(folder):
    spec=importlib.util.spec_from_file_location(folder,ROOT/'tools'/folder/'protocol.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


inp,wm,media,uc=map(load,('native-input','dtvwm','native-media','native-uconnect'))


class InputTests(unittest.TestCase):
    def test_menu_golden(self):
        # Existing menu_hook.c frame; owner chosen as 42 for the fixture.
        self.assertEqual(inp.register(42,exclusive=[0xe503]).hex(),
                         '0000001c000000000000002a0000000100000001000000020000e503')
    def test_session_golden(self):self.assertEqual(inp.session().hex(),'0000000c0000001000000000')
    def test_multi_key_counts(self):
        d=inp.decode(inp.register(42,shared=[0xe100,0xe101],exclusive=[0xe503,0xe402]))
        self.assertEqual(d['shared'],[0xe100,0xe101]);self.assertEqual(d['exclusive'],[0xe503,0xe402])
    def test_release_replacement(self):
        self.assertEqual(len(inp.register(42)),24)
        self.assertEqual(inp.decode(inp.register(42))['exclusive'],[])
    def test_close_and_push(self):
        self.assertEqual(inp.close(42),inp.words(12,4,42))
        self.assertEqual(inp.push(42,0x1e503),inp.words(16,5,42,0x1e503))
    def test_registration_rejects_duplicates_and_press_mask(self):
        for args in ({'shared':[0xe503],'exclusive':[0xe503]},{'exclusive':[0]},{'exclusive':[0x1e503]}):
            with self.assertRaises(ValueError):inp.register(42,**args)
    def test_event_big_endian(self):
        d=inp.decode(bytes.fromhex('0000001c000000080001e50300000002010203040506070800000001'))
        self.assertEqual((d['key'],d['pressed'],d['session'],d['timestamp'],d['device']),
                         (0xe503,True,2,0x0102030405060708,1))
    def test_legacy_event(self):self.assertFalse(inp.decode(inp.frame(8,0xe503))['pressed'])
    def test_ack(self):
        self.assertTrue(inp.decode(inp.frame(7,1))['accepted'])
        self.assertEqual(inp.decode(inp.frame(7,3,0xe503,0xe00b))['rejected'],[0xe503,0xe00b])
    def test_malformed(self):
        for b in (b'',inp.words(16,8,0xe503),inp.words(16,0,42,1),inp.words(24,0,42,1,0,1),inp.frame(7,2),inp.frame(8)):
            with self.subTest(packet=b),self.assertRaises(ValueError):inp.decode(b)
    def test_fragmentation_and_coalescing(self):
        a=inp.frame(7,1);b=inp.frame(8,0x1e503);s=inp.StreamDecoder()
        self.assertEqual(s.feed(a[:3]),[])
        self.assertEqual(len(s.feed(a[3:]+b)),2)
    def test_bad_stream_length(self):
        with self.assertRaises(ValueError):inp.StreamDecoder().feed(inp.words(0xffffffff))
    def test_repeat_and_release(self):
        t=inp.KeyTracker();press=inp.decode(inp.frame(8,0x1e503));release=inp.decode(inp.frame(8,0xe503))
        self.assertFalse(t.consume(press)['repeated']);self.assertTrue(t.consume(press)['repeated'])
        self.assertFalse(t.consume(release)['repeated']);self.assertFalse(t.consume(press)['repeated'])


class WindowTests(unittest.TestCase):
    def test_registration(self):
        r=wm.registration(2,123);self.assertEqual(len(r),64)
        self.assertEqual(struct.unpack('>16I',r),(1,2,123)+tuple([0]*13))
    def test_configuration_golden(self):
        self.assertEqual(wm.configure_surface(2,4,720,480).hex(),
            '00000008d123567a00000010000000000000000200000004000002d0000001e0')
    def test_split_payload(self):
        d=wm.decode_request(wm.request(20,b'1234',b'567'))
        self.assertEqual((d['data'],d['additional']),(b'1234',b'567'))
    def test_no_alignment_padding(self):self.assertEqual(len(wm.request(92,additional=b'abc')),19)
    def test_opaque_response(self):
        r=wm.response(wm.words(3,27,*range(14)))
        self.assertEqual(r['status'],3);self.assertEqual(r['result'],27);self.assertEqual(r['words'][-1],13)
    def test_invalid_requests(self):
        for b in (b'',wm.words(8,0,0,0),wm.words(112,wm.COOKIE,0,0),wm.words(8,wm.COOKIE,4,0)):
            with self.assertRaises(ValueError):wm.decode_request(b)
    def test_invalid_response(self):
        with self.assertRaises(ValueError):wm.response(b'\0'*60)
    def test_signed_z(self):self.assertEqual(wm.decode_request(wm.set_z_position(2,-1))['data'][-4:],b'\xff'*4)


class MediaTests(unittest.TestCase):
    def test_pause_golden(self):
        self.assertEqual(media.message('PlayRequest',7,Rate=0).hex(),
                         '00000018ef91820b000000070000000c4146afde00000000')
    def test_resume(self):self.assertEqual(media.decode(media.message('PlayRequest',Rate=1000))['fields'][0][1],b'\0\0\x03\xe8')
    def test_reverse_rate(self):self.assertEqual(media.decode(media.message('PlayRequest',Rate=-6000))['fields'][0][1],struct.pack('>i',-6000))
    def test_position_request(self):self.assertEqual(media.message('GetPositionRequest').hex(),'0000000cb54067a000000000')
    def test_seek_long_signed(self):
        d=media.decode(media.message('JumpRequest',OriginType=1,Position=-1000,MaintainSpeed=True))
        self.assertEqual(d['fields'][1],(0x955e4fbf,struct.pack('>q',-1000)))
        self.assertEqual(len(d['fields'][2][1]),1)
    def test_unaligned_boolean(self):
        # Two 9-byte fields; no invented 32-bit padding between them.
        p=media.message('PlayResponse',Result=True,Retry=False)
        self.assertEqual(len(p),30);self.assertEqual(media.decode(p)['fields'][1],(0xa1e7518e,b'\0'))
    def test_duplicate_tags_preserved(self):
        self.assertEqual(media.decode(media.encode(1,fields=[(9,b'a'),(9,b'b')]))['fields'],[(9,b'a'),(9,b'b')])
    def test_malformed_tlv(self):
        for b in (b'',struct.pack('>3I',16,1,0),struct.pack('>5I',20,1,0,7,9),struct.pack('>5I',20,1,0,100,9)):
            with self.assertRaises(ValueError):media.decode(b)
    def test_invalid_schema_values(self):
        for args in ({'Rate':1<<31},{'Unknown':0}):
            with self.assertRaises(ValueError):media.message('PlayRequest',**args)


class UconnectTests(unittest.TestCase):
    def test_xml_escape(self):
        value='http://offline.invalid/?a=1&b="quoted"'
        e=ET.fromstring(uc.request('playURL',{'url':value}))
        self.assertEqual(e.attrib['url'],value);self.assertEqual(e.tag,uc.DIRECTTEST)
    def test_command_collision(self):
        with self.assertRaises(ValueError):uc.request('a',{'command':'b'})
    def test_envelope_golden(self):
        self.assertEqual(uc.envelope(1,blob=b'<x/>',flags=0xc0000000).hex(),
                         '0000001400000001c0000000000000003c782f3e')
    def test_words_blob(self):
        d=uc.decode(uc.envelope(123,[1,0xffffffff],b'abc',0x80000000))
        self.assertEqual(d['words'],[1,0xffffffff]);self.assertEqual(d['blob'],b'abc')
    def test_bad_envelope(self):
        for b in (b'',struct.pack('>4I',16,0,1,0),struct.pack('>4I',17,0,0,0)):
            with self.assertRaises(ValueError):uc.decode(b)


if __name__=='__main__':unittest.main()
