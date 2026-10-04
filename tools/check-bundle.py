#!/usr/bin/env python3
"""Read-only artifact/privacy checks and early installer refusal tests."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
payload = root / 'receiver/payload/hr54-persist'
expected = {
    'native-menu/hr54-ui': 'e014f494fa7a056666f4709c09632419',
    'native-menu/hr54-input-broker': '5524b8e01569cf8e66b7586a03874eba',
    'jellyfin/bin/hr54-jf': '406e984b2c26e0ae1832b6d122238da3',
    'bin/hr54-play-url': '853fe5b3c7c6e0a61160af438b6cece0',
    'native-menu/dtv-menu-policy.car': 'c1b9e5ba4546c42f799cf580f048afb1',
    'jellyfin/doom/bin/hr54-doom-native': 'dc8d44e254cedc79e39c21036d36eedd',
}
for name, digest in expected.items():
    assert hashlib.md5((payload/name).read_bytes()).hexdigest() == digest, name
apk = root/'android/ReBox-controller.apk'
assert hashlib.sha256(apk.read_bytes()).hexdigest() == 'd41c7684fecafd696fb07acb8e6122cf2719aec59a2535fe35209b2494d3cf81'
with zipfile.ZipFile(apk) as package:
    assert any(s.startswith('lib/arm64-v8a/') for s in package.namelist())
assert (payload/'jellyfin/iptv/eng.m3u').read_text().startswith('#EXTM3U')
assert '--cookies' not in (payload/'jellyfin/youtube/yt-dlp.conf').read_text()
for directory in ['jellyfin/config', 'jellyfin/state', 'jellyfin/cache']:
    assert not list((payload/directory).iterdir()), directory
for flag in ['ENABLED','ACTIVATED','BROKER_BYPASSED','loaded-boot']:
    assert not (payload/'native-menu'/flag).exists(), flag
for file in root.rglob('*'):
    if file.is_symlink():
        assert file.resolve().is_relative_to(root), file
    assert file.name not in ['cookies.txt','debug.keystore','local.properties','phone-controller-settings-before.tar'], file
for shell in ['tools/prepare-stock-image.sh','tools/activate-native.sh','tools/deactivate-native.sh','stock-bootstrap/guest-init','stock-bootstrap/indexer','receiver/payload/hr54-persist/rebox-start.sh']:
    interpreter = 'bash' if shell.endswith('prepare-stock-image.sh') else 'sh'
    subprocess.run([interpreter,'-n',str(root/shell)], check=True)
with tempfile.TemporaryDirectory(prefix='rebox-guard-test-') as td:
    image = Path(td)/'wrong-size.img'
    image.write_bytes(b'not a disk')
    command = [str(root/'tools/prepare-stock-image.sh'),'audit']
    for target in ['/dev/null',str(image)]:
        result = subprocess.run(command+[target,str(root/'stock-bootstrap/rebox.conf.example')],capture_output=True,text=True)
        assert result.returncode != 0 and 'Work directory' not in result.stdout, result.stdout
    print('PASS physical-device/wrong-size inputs rejected before VM/image writes')
print('PASS accepted artifact identities, APK ABI, no accounts/cookies/state/keys, inactive native markers, internal symlinks, shell syntax')
