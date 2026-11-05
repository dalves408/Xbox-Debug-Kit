from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import tempfile

toolkit=Path(sys.argv[1])
script=Path(__file__).resolve().parents[1]/'generate_runtime.py'
paths=['src/kernel/kernel_bridge.c','src/kernel/kernel_sync.c','src/kernel/xbox_memory_layout.c','src/apu/apu_core.c','src/usb/ohci.c','src/kernel/kernel_rtl.c']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
before={name:digest(toolkit/name) for name in paths}
with tempfile.TemporaryDirectory(prefix='xrx-runtime-test-') as directory:
    work=Path(directory);output=work/'output'
    command=[sys.executable,str(script),str(toolkit),str(output)]
    subprocess.run(command,check=True)
    generated=list(output.glob('*.c'));assert len(generated)==6
    times={p.name:p.stat().st_mtime_ns for p in generated}
    subprocess.run(command,check=True)
    assert times=={p.name:p.stat().st_mtime_ns for p in generated},'unchanged runtime was rewritten'
    assert before=={name:digest(toolkit/name) for name in paths},'shared toolkit source changed'
    sync=(output/'kernel_sync_state.c').read_text(encoding='utf-8')
    assert 'xbox_NtWaitForSingleObjectEx(' in sync and 'xrx_state_wait_single(' in sync
    rtl=(output/'kernel_rtl_state.c').read_text(encoding='utf-8')
    assert 'xrx_state_guest_checkpoint();' in rtl and 'EnterCriticalSection(cs);' not in rtl.split('xbox_GuestCpuBlocked();',1)[1].split('}',1)[0]
    fixture=work/'fixture'
    for name in paths:
        target=fixture/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(toolkit/name,target)
    bridge=fixture/paths[0]
    text=bridge.read_text(encoding='utf-8').replace('void guest_cpu_join(void)','void changed_guest_cpu_join(void)')
    bridge.write_text(text,encoding='utf-8')
    failed=subprocess.run([sys.executable,str(script),str(fixture),str(work/'mismatch')],capture_output=True,text=True)
    assert failed.returncode!=0 and 'Runtime hook mismatch' in failed.stderr,'changed toolkit did not fail closed'
print('PASS: generated runtime preserves source, supports incremental builds, retains Xbox API names and rejects incompatible hooks')
