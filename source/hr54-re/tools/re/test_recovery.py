#!/usr/bin/env python3
"""Cross-check recovery against the extracted artifacts, entirely offline."""
import json
import unittest
from pathlib import Path
from car_classes import Car
from dtvwm_inventory import inventory,server_table
from mips_elf import Elf

ROOT=Path(__file__).resolve().parents[2]
FS=ROOT/'extracted/sdb4-rootfs'


@unittest.skipUnless((FS/'opt/dtv/dtv.car').is_file(),'extracted fixture unavailable')
class RecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.car=Car(FS/'opt/dtv/dtv.car')
    def test_all_generated_message_class_bounds(self):
        s=json.loads((ROOT/'docs/native-evidence/media-schemas.json').read_text())
        self.assertEqual(len(s['schemas']),116);self.assertEqual(s['failures'],[])
        for row in s['schemas']:
            d=self.car.describe(row['class_name'])
            self.assertEqual(d['offset'],row['offset'])
    def test_native_and_bytecode_classes(self):
        raw=self.car.describe('com/directv/mw/bsp/keydispatcher/RawKeyDispatcher')
        self.assertEqual(next(m for m in raw['methods'] if m['name']=='OpenKeyInput')['code'],'')
        proxy=self.car.describe('com/directv/dvrcore/impl/MediaPlayer/MediaPlayerProxy')
        overload=next(m for m in proxy['methods'] if m['name']=='play' and ';J)' in m['descriptor'])
        self.assertEqual(overload['code'],'03ac')
    def test_dtvwm_server_dispatch(self):
        rows={r['command']:r for r in server_table(Elf(FS/'opt/dtvwm/lib/libdtvwm.so'))}
        self.assertEqual(len(rows),105)
        self.assertIn('disableSleepModeSupport',rows[109]['handler'])
        self.assertIn('disableJsonGraphicsSupport',rows[110]['handler'])
        self.assertEqual(rows[7]['handler_address'],'0x5fa80')
    def test_dtvwm_client_known_constructors(self):
        rows=inventory(Elf(FS/'opt/dtvwm/lib/libdtvwmipcclient.so'))
        self.assertTrue(any(r['command']==8 and r['data_bytes']==16 for r in rows))
        self.assertTrue(any(r['command']==109 and 'disableJsonGraphicsSupport' in r['method'] for r in rows))
    def test_vtable_relocation_set_rate(self):
        e=Elf(FS/'opt/dvr_core/lib/libdvr.so')
        address=0xa1e4e0+0x1c
        v=int.from_bytes(e.read(address,4),'big')
        if address in e.relocations:v=(v+e.relocations[address][0])&0xffffffff
        self.assertEqual(e.byaddr[v]['demangled'],'libdvr::LocalPlayer::setRate(libdvr::Rate)')


if __name__=='__main__':unittest.main()
