#!/usr/bin/env python3
"""Build real receiver binaries and trusted offline packages; no device writes.

The provider list here is distribution metadata, never runtime dispatch.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'source/hr54-re'
PAYLOAD = ROOT / 'receiver/payload/hr54-persist'
PROVIDERS = ('jellyfin', 'iptv', 'youtube', 'frigate', 'doom')
spec = importlib.util.spec_from_file_location('package_builder', ROOT / 'tools/build-module.py')
builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)

def copy(source, dest, executable=False):
    source = Path(source); dest = Path(dest)
    if source.is_symlink(): raise ValueError('Bundle source must be regular: ' + str(source))
    if source.is_dir():
        dest.mkdir(parents=True, exist_ok=True); dest.chmod(0o700)
        for child in sorted(source.iterdir()): copy(child, dest / child.name, bool(child.stat().st_mode & 0o111))
    else:
        dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, dest); dest.chmod(0o700 if executable else 0o600)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists(): raise ValueError('Refusing to replace existing bundle output')
    env = {**os.environ, 'HR54_SYSROOT': str(args.sysroot.resolve()), 'ZIG_GLOBAL_CACHE_DIR': '/tmp/rebox-zig-cache', 'ZIG_LOCAL_CACHE_DIR': '/tmp/rebox-zig-local'}
    def run(command): subprocess.run(command, cwd=ROOT, env=env, check=True)
    run(['sh', str(SRC / 'reboxd/build-receiver.sh')])
    for identity in PROVIDERS: run(['sh', str(SRC / 'modules' / identity / 'build-receiver.sh')])
    # Native UI uses the existing extracted graphics ABI; its firmware is checked at deployment.
    ui_env = {**env, 'HR54_SYSROOT': str(SRC / 'extracted/sdb4-rootfs')}
    subprocess.run(['sh', str(SRC / 'hr54-ui/tools/build-receiver.sh')], cwd=ROOT, env=ui_env, check=True, stdout=subprocess.DEVNULL)
    out.mkdir(parents=True); out.chmod(0o700)
    runtime = out / 'rebox'; runtime.mkdir(); runtime.chmod(0o700)
    copy(SRC / 'reboxd/build/receiver/reboxd', runtime / 'bin/reboxd', True)
    copy(SRC / 'reboxd/build/receiver/module-fetch', runtime / 'bin/module-fetch', True)
    copy(SRC / 'reboxd/build/receiver/module-sha', runtime / 'bin/module-sha', True)
    copy(PAYLOAD / 'jellyfin/iptv/ca-certificates.crt', runtime / 'ca-certificates.crt')
    entries = []
    for identity in PROVIDERS:
        package = runtime / 'modules' / identity
        copy(SRC / 'modules' / identity / 'module.json', package / 'module.json')
        copy(SRC / 'modules' / identity / 'build/bin/module', package / 'bin/module', True)
        copy(SRC / 'hr54-ui/assets/icons' / (identity + '.png'), package / 'icon.png')
        manifest = json.loads((package / 'module.json').read_text()); manifest['icon'] = 'icon.png'
        (package / 'module.json').write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
        if identity == 'iptv':
            copy(SRC / 'modules/iptv/build/bin/fetch', package / 'bin/fetch', True)
            copy(PAYLOAD / 'jellyfin/iptv', package / 'default-data/iptv')
        elif identity == 'youtube':
            default = package / 'default-data/youtube'
            for name in ('python', 'qjs', 'remux', 'exec-guard'):
                copy(PAYLOAD / 'jellyfin/youtube/bin' / name, default / 'bin' / name, True)
            for name in ('python', 'yt-dlp.zip', 'yt-dlp.conf', 'version'):
                copy(PAYLOAD / 'jellyfin/youtube' / name, default / name)
            copy(PAYLOAD / 'jellyfin/iptv/ca-certificates.crt', default / 'ca-certificates.crt')
            copy(SRC / 'modules/youtube/build/bin/relay', package / 'bin/relay', True)
            copy(SRC / 'modules/iptv/build/bin/fetch', package / 'bin/fetch', True)
            # The receiver runtime is Python 3.14; use the matching bytecode compiler.
            for name in ('resolver', 'updater'):
                run(['python3.14', '-c', 'import py_compile,sys;py_compile.compile(sys.argv[1],cfile=sys.argv[2],dfile=sys.argv[3],doraise=True,invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)', str(SRC / 'modules/youtube' / (name + '.py')), str(package / 'bin' / (name + '.pyc')), name + '.py'])
        elif identity == 'doom':
            copy(PAYLOAD / 'jellyfin/doom/bin/hr54-doom-native', package / 'bin/hr54-doom-native', True)
            copy(PAYLOAD / 'jellyfin/doom/data', package / 'default-data/doom/data')
            copy(PAYLOAD / 'jellyfin/doom/LICENSE.txt', package / 'LICENSE.txt')
        filename = identity + '.rbox'
        digest = builder.package(package, runtime / 'builtin' / filename)
        entries.append(dict(id=identity, name=manifest['name'], package=filename, sha256=digest, defaultEnabled=True))
    (runtime / 'builtin/catalog.json').write_text(json.dumps({'schema': 1, 'modules': entries}, indent=2) + '\n')
    copy(SRC / 'hr54-ui/build/hr54-ui', out / 'hr54-ui', True)
    # Distribution integrity hashes are calculated from these actual bytes.
    files = sorted(p for p in out.rglob('*') if p.is_file())
    (out / 'bundle.md5').write_text(''.join(hashlib.md5(p.read_bytes()).hexdigest() + '  ' + p.relative_to(out).as_posix() + '\n' for p in files))
    (out / 'bundle.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.relative_to(out).as_posix() + '\n' for p in files))
    for p in out.rglob('*'):
        if p.is_file() and not p.stat().st_mode & 0o111: p.chmod(0o600)
    print('BUILT', out)
if __name__ == '__main__': main()
