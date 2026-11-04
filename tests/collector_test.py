"""Isolated collector check; requires Windows PowerShell and no game installation."""
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

script = Path(__file__).resolve().parents[1] / 'installer' / 'collect_debug.ps1'
with tempfile.TemporaryDirectory(prefix='xrdbg-test-') as directory:
    work = Path(directory)
    base = work / 'appdata' / 'Fixture'
    debug = base / 'debug'
    debug.mkdir(parents=True)
    (work / 'temp').mkdir()
    (debug / 'session.log').write_text('fixture session')
    (debug / 'graphics.txt').write_text('actual render GPU: debug session')
    (base / 'graphics.txt').write_text('old release GPU report')
    (base / 'settings.ini').write_text('scale=2')
    env = dict(os.environ, LOCALAPPDATA=str(work/'appdata'), TEMP=str(work/'temp'), XRDBG_COLLECT_QUIET='1')
    for current in (True, False):
        output = work / ('current' if current else 'fallback')
        output.mkdir()
        if not current:
            (debug/'graphics.txt').unlink()
        result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                                 '-DataFolder', 'Fixture', '-GameName', 'Fixture Game', '-OutputDirectory', str(output)],
                                env=env, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stderr
        archives = list(output.glob('Fixture-debug-*.zip'))
        assert len(archives) == 1, result.stdout + result.stderr
        with zipfile.ZipFile(archives[0]) as archive:
            contents = {Path(n).name: archive.read(n).decode() for n in archive.namelist() if not n.endswith('/')}
            assert contents['graphics.txt'] == ('actual render GPU: debug session' if current else 'old release GPU report')
            assert contents['session.log'] == 'fixture session'
            assert contents['settings.ini'] == 'scale=2'
            assert 'GPU inventory' in contents['computer.txt']
        assert not list((work/'temp').glob('xrdbg_*')), 'staging folder was not cleaned up'
print('PASS: chosen output folder, session GPU report precedence, fallback, settings/logs and staging cleanup')
