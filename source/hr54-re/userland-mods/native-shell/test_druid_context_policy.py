#!/usr/bin/env python3
"""Run production shell logic against isolated files and a typed idle-check double."""
from pathlib import Path
import os
import stat
import subprocess
import tempfile

source = Path(__file__).with_name('druid-context-policy.sh').read_text()
with tempfile.TemporaryDirectory(prefix='hr54-druid-policy-') as td:
    base = Path(td)
    registry = base / 'registry' / 'directv.DRUID'
    menu = base / 'native-menu'
    registry.mkdir(parents=True)
    menu.mkdir()
    auto = registry / 'Auto-Start.str'
    main = registry / 'Context-Main.str'
    sentinel = registry / 'Never-Change.str'
    sibling = registry.parent / 'directv.BSP'
    sibling.mkdir()
    (sibling / 'Auto-Start.str').write_bytes(b'true')
    sentinel.write_bytes(b'headless and context order untouched')
    auto.write_bytes(b'true')
    auto.chmod(0o640)
    main.write_bytes(b'com.directv.druid.DruidMain')
    ui = menu / 'hr54-ui'
    ui.write_text('''#!/bin/sh
[ "$*" = '--check-api --idle' ] || exit 9
[ "$POLICY_API_STATE" = idle ] || exit 1
''')
    ui.chmod(0o700)
    script = base / 'policy.sh'
    script.write_text(source.replace('/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID', str(registry))
                           .replace('/var/hr54-persist/native-menu', str(menu)))
    commands = base / 'commands'
    commands.mkdir()
    # Test root-only guard independently of the account executing this suite.
    identity = commands / 'id'
    identity.write_text('#!/bin/sh\nprintf "%s\\n" "${POLICY_TEST_UID:-0}"\n')
    identity.chmod(0o700)
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'], POLICY_API_STATE='idle')

    def run(*args, state='idle', uid='0', success=True):
        result = subprocess.run(['sh', str(script), *args], text=True,
                                capture_output=True, timeout=10,
                                env=dict(env, POLICY_API_STATE=state, POLICY_TEST_UID=uid))
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        assert sentinel.read_bytes() == b'headless and context order untouched'
        assert (sibling / 'Auto-Start.str').read_bytes() == b'true'
        assert main.read_bytes() in (b'com.directv.druid.DruidMain', b'wrong.Main')
        assert not list(registry.glob('.Auto-Start.native.*'))
        assert not (menu / 'druid-context-policy.lock').exists()
        return result

    def backups():
        return set((menu / 'backup').glob('druid-context.*')) if (menu / 'backup').exists() else set()

    assert 'configured_auto_start=true' in run('--status', state='down').stdout
    for state in ('media', 'game', 'down', 'malformed'):
        run('--disable', state=state, success=False)
        assert auto.read_bytes() == b'true' and not backups()
    run('--disable', uid='1000', success=False)
    ui.chmod(0o600)
    run('--disable', success=False)
    ui.chmod(0o700)
    main.write_bytes(b'wrong.Main')
    run('--disable', success=False)
    main.write_bytes(b'com.directv.druid.DruidMain')
    for invalid in (b'unknown', b'true\n', b'false\n\n'):
        auto.write_bytes(invalid)
        run('--disable', success=False)
        assert auto.read_bytes() == invalid and not backups()
    auto.write_bytes(b'true')
    old_mode = stat.S_IMODE(auto.stat().st_mode)
    result = run('--disable')
    original = next(iter(backups()))
    assert str(original) in result.stdout
    assert auto.read_bytes() == b'false'
    assert (original / 'Auto-Start.str').read_bytes() == b'true'
    assert stat.S_IMODE(auto.stat().st_mode) == old_mode
    run('--disable')
    assert backups() == {original}
    run('--restore', str(base), success=False)
    run('--restore', str(original), state='media', success=False)
    assert auto.read_bytes() == b'false'
    run('--restore', str(original))
    assert auto.read_bytes() == b'true' and len(backups()) == 2
    reverse = (backups() - {original}).pop()
    assert (reverse / 'Auto-Start.str').read_bytes() == b'false'
    run('--restore', str(reverse))
    assert auto.read_bytes() == b'false' and len(backups()) == 3
    (original / 'target').write_bytes(b'/another/registry')
    run('--restore', str(original), success=False)
    assert auto.read_bytes() == b'false'
    auto.unlink()
    auto.symlink_to(reverse / 'Auto-Start.str')
    run('--disable', success=False)
    assert auto.is_symlink()
print('PASS read-only status, exact context/string/root guards, API idle fail-closed, unique backup, reversible restore, metadata and sibling preservation')
