#!/usr/bin/env python3
"""Regenerate key native-control evidence from local extracted files only.

Runs host Python, readelf, c++filt and javap. Never runs a receiver binary,
opens a socket, accesses a device, mounts an image or deploys anything.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FS = ROOT / 'extracted/sdb4-rootfs'
SOURCES = [
    'opt/key_dispatcher/bin/keydispatcher',
    'opt/key_dispatcher/lib/libuserinput.so', 'opt/vm/siege',
    'opt/itv/itvpack/root/lib/libcwebkit.so',
    'opt/dtvwm/bin/dtvwm', 'opt/dtvwm/lib/libdtvwm.so',
    'opt/dtvwm/lib/libdtvwmipcclient.so', 'opt/dtvwm/lib/libdtvwmbase.so',
    'opt/middleware_core/lib/libump.so',
    'opt/middleware_core/lib/libuconnect.so',
    'opt/middleware_core/system/tv/uconntest',
    'opt/dtv/dtv.car', 'opt/dvr_core/bin/dvr_core',
    'opt/dvr_core/lib/libdvr.so', 'opt/binaryipc/lib/libbinaryipc.so',
]


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True,
                   help='new local evidence directory; must not already exist')
    a = p.parse_args()
    for name in SOURCES:
        if not (FS / name).is_file():
            p.error('missing extracted regular file: ' + name)
    out = a.output.resolve()
    if out.exists():
        p.error('output already exists; choose a new directory')
    out.mkdir(parents=True)
    commands = []

    def run(filename, script, *args):
        argv = [sys.executable, str(ROOT / 'tools/re' / script),
                *map(str, args)]
        with (out / filename).open('w') as f:
            subprocess.run(argv, cwd=ROOT, stdout=f, check=True)
        commands.append({'output': filename,
                         'argv': [sys.executable, 'tools/re/' + script,
                                  *[str(x).replace(str(ROOT) + '/', '') for x in args]]})

    run('dtvwm-commands.json', 'dtvwm_inventory.py',
        FS / 'opt/dtvwm/lib/libdtvwmipcclient.so')
    run('dtvwm-server-table.json', 'dtvwm_inventory.py',
        FS / 'opt/dtvwm/lib/libdtvwm.so', '--server')
    car = FS / 'opt/dtv/dtv.car'
    run('media-schemas.json', 'car_inventory.py', car, 'schemas')
    run('uconnect-commands.json', 'car_inventory.py', car, 'commands')
    for filename, classes in [
        ('DirectTest.javap', ['com/ucentric/pvruconnect/DirectTest']),
        ('MediaPlayerProxy.javap', ['com/directv/dvrcore/impl/MediaPlayer/MediaPlayerProxy']),
        ('MediaPlayer.javap', ['com/directv/mw_core/mediaplayer/MediaPlayer']),
        ('TrickPlayController.javap', ['com/directv/mw_core/mediaplayer/TrickPlayController']),
        ('input-java-stock.javap', ['com/directv/mw/bsp/keydispatcher/RawKeyDispatcher',
                                   'com/directv/mw/bsp/keydispatcher/KeyEventTranslator']),
        ('key-rates.javap', ['com/directv/shef/util/KeyTranslator',
                            'com/ucentric/pvrdirect/PlayMode',
                            'com/directv/dvrcore/api/PositionOrigin']),
    ]:
        run(filename, 'car_classes.py', car, *classes, '--javap')
    for mode, filename in [('dtvwm', 'dtvwm-protocol-table.md'),
                           ('media', 'media-schema-table.md'),
                           ('keys', 'input-keys.md')]:
        run(filename, 'native_reports.py', mode, '--evidence', out)
    for filename, source, start, end in [
        ('input-stack-predecessor.asm', 'opt/key_dispatcher/bin/keydispatcher', '0x405bdc', '0x405dc0'),
        ('input-push-routing.asm', 'opt/key_dispatcher/bin/keydispatcher', '0x407000', '0x407238'),
        ('input-push-handler.asm', 'opt/key_dispatcher/bin/keydispatcher', '0x408de4', '0x408ebc'),
        ('media-dispatch.asm', 'opt/dvr_core/bin/dvr_core', '0x454f2c', '0x457368'),
    ]:
        run(filename, 'mips_elf.py', FS / source, 'disasm', '--start', start, '--end', end)
    manifest = {
        'scope': 'Offline extracted-file analysis; no receiver execution or interaction',
        'inputs': [{'path': str((FS / name).relative_to(ROOT)),
                    'bytes': (FS / name).stat().st_size, 'sha256': sha(FS / name)}
                   for name in SOURCES],
        'commands': commands,
        'outputs': [{'path': str(f.relative_to(out)), 'bytes': f.stat().st_size,
                     'sha256': sha(f)} for f in sorted(out.iterdir()) if f.is_file()],
    }
    (out / 'reproduction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Regenerated {len(commands)} evidence files in {out}')


if __name__ == '__main__':
    main()
