"""Bounded state run with isolated settings and saves.

Default: staged native capture probe (--delay). --schedule "ms:save,ms:load,..."
drives real frame-boundary save/load (XRDBG_STATE_SCHEDULE) and checks that each
load restored the RAM hash of the save before it."""
import re
from pathlib import Path
from datetime import datetime
import os
import subprocess
import json
import time
import argparse
parser=argparse.ArgumentParser()
parser.add_argument("--exe",required=True,help="the debug build of the game")
parser.add_argument("--delay",default="30000")
parser.add_argument("--script",required=True,help="RECOMP_PAD_SCRIPT file")
parser.add_argument("--echo",action="store_true")
parser.add_argument("--seconds",type=int,default=48)
parser.add_argument("--schedule",default="")
args=parser.parse_args()

exe=Path(args.exe).resolve();build=exe.parent.parent
run=build/(('state-run-' if args.schedule else 'native-probe-')+datetime.now().strftime('%Y%m%d-%H%M%S'))
run.mkdir()
(run/'appdata').mkdir()
(run/'saves').mkdir()
(run/'shots').mkdir()
env=dict(os.environ,LOCALAPPDATA=str(run/'appdata'),RECOMP_SAVE_DIR=str(run/'saves'),
         XRDBG_STATE_NATIVE_PROBE_MS=str(args.delay),RECOMP_PAD_SCRIPT='@'+str(Path(args.script).resolve()),XRDBG_STATE_SHOT_DIR=str(run/'shots'))
if args.schedule:
    env['XRDBG_STATE_SCHEDULE']=args.schedule; env.pop('XRDBG_STATE_NATIVE_PROBE_MS',None)
else: env.pop('XRDBG_STATE_SCHEDULE',None)
if args.echo: env["XRDBG_STATE_NATIVE_PROBE_RESTORE"]="1"
else: env.pop("XRDBG_STATE_NATIVE_PROBE_RESTORE",None)
startup=subprocess.STARTUPINFO()
startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow=0
with (run/'stdout.log').open('wb') as out,(run/'stderr.log').open('wb') as err:
    p=subprocess.Popen([str(exe)],
                       cwd=exe.parent,env=env,stdout=out,stderr=err,startupinfo=startup)
    print(f'Capture-only test PID {p.pid}; evidence {run}',flush=True)
    start=time.monotonic()
    try:
        while p.poll() is None and time.monotonic()-start<args.seconds:
            try:p.wait(timeout=1)
            except subprocess.TimeoutExpired:pass
        still_running=p.poll() is None
    finally:
        if p.poll() is None:
            p.terminate()
            try:p.wait(timeout=10)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
    logs=list((run/'appdata').glob('*/debug/session_*.log'))
    lines=[]
    for log in logs:
        for line in log.read_text(encoding='utf-8',errors='replace').splitlines():
            if '[STATE]' in line or '[HB' in line or '[FATAL]' in line or '[CRASH]' in line:
                lines.append(line)
    # Disc reads per phase (between scheduled ops): a stream that stops after a load is a failed restore the hashes cannot see.
    read_phases=[]
    for log in logs:
        n=0;name='start'
        for line in log.read_text(encoding='utf-8',errors='replace').splitlines():
            m=re.search(r'scheduled (save|load) at (\d+)',line)
            if m: read_phases.append(dict(phase=name,reads=n));n=0;name=m.group(1)+'@'+m.group(2)
            elif '[READ]' in line: n+=1
        read_phases.append(dict(phase=name,reads=n))
    pending=None;saved=None;checks=[]
    for line in lines:
        if '] save: ok' in line.lower(): saved=pending  # the capture that became the slot (loads first take a rollback capture)
        m=re.search(r'\[STATE\] (capture|restore): flip=(\d+) ram=([0-9A-F]+) frame=([0-9A-F]+)',line)
        if not m: continue
        if m.group(1)=='capture': pending=dict(ram=m.group(3),frame=m.group(4),flip=int(m.group(2)))
        elif saved: checks.append(dict(ram_match=m.group(3)==saved['ram'],frame_match=m.group(4)==saved['frame'],
                                 snapshot=saved,restored=dict(ram=m.group(3),frame=m.group(4),flip=int(m.group(2)))))
    visuals=[]
    try:
        from PIL import Image, ImageChops, ImageStat
        def mad(a,b): return round(sum(ImageStat.Stat(ImageChops.difference(a,b)).mean[:3])/3,2)
        sh=run/'shots'
        for n in range(1,100):
            names=[f'save-{n:02d}-k0',f'save-{n:02d}-k10',f'load-{n:02d}-k0',f'load-{n:02d}-k10']
            if not all((sh/(x+'.bmp')).exists() for x in names): continue
            im=[Image.open(sh/(x+'.bmp')).convert('RGB') for x in names]
            for x,i in zip(names,im): i.save(sh/(x+'.jpg'),quality=85)
            # k0: saved vs restored moment; kK: K frames after save vs after load; motion: load k0 vs kK
            visuals.append(dict(cycle=n,k0_diff=mad(im[0],im[2]),kK_diff=mad(im[1],im[3]),motion_after_load=mad(im[2],im[3])))
    except ImportError: pass
    summary=dict(pid=p.pid,saves_ok=sum('] save: ok' in l.lower() for l in lines),loads_ok=sum('] load: ok' in l.lower() for l in lines),
                 load_checks=checks,all_loads_match=bool(checks) and all(c['ram_match'] and c['frame_match'] for c in checks),
                 fatal=any('[FATAL]' in l or '[CRASH]' in l for l in lines),ran_seconds=round(time.monotonic()-start,2),alive_at_test_end=still_running,
                 exit_code=p.returncode,read_phases=read_phases,visuals=visuals,diagnostics=lines)
    (run/'result.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary,indent=2),flush=True)
