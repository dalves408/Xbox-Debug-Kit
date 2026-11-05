"""Generate debug-only instrumented runtime copies; never edit shared toolkit sources."""
from pathlib import Path
import sys
import re

toolkit, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Runtime hook mismatch (expected one occurrence): {old[:100]!r}')
    return text.replace(old,new)
def write(name,text):
    p=output/name
    if not p.exists() or p.read_text(encoding="utf-8")!=text: p.write_text(text,encoding='utf-8',newline='\n')


def function(text, signature, transform):
    start=text.index(signature)
    end=text.index('\n}\n',start)+3
    return text[:start]+transform(text[start:end])+text[end:]

s=(toolkit/'src/kernel/kernel_bridge.c').read_text(encoding="utf-8")
s='#include "xrx_state_runtime.h"\n'+s
s=replace(s,'void guest_cpu_join(void)\n{\n    xbox_GuestCpuJoin();',
          'void guest_cpu_join(void)\n{\n    xrx_state_guest_join();\n    xbox_GuestCpuJoin();')
s=replace(s,'    xbox_HostCodeLeave(1);  /* parks here, holding nothing, if a suspend came in meanwhile */',
          '    xbox_HostCodeLeave(1);  /* parks here, holding nothing, if a suspend came in meanwhile */\n    xrx_state_guest_checkpoint(); /* guest CPU released; kernel call completed */')
s=replace(s,'    g_thread_obj_va = 0;\n    return 0;\n}',
          '    g_thread_obj_va = 0;\n    xrx_state_guest_part();\n    return 0;\n}')
s=replace(s,'        ExitThread(exit_status);','        xrx_state_guest_part();\n        ExitThread(exit_status);')
s=replace(s,'    while (InterlockedCompareExchange(&s->parked, 0, 0) > 0)\n        WaitForSingleObject(s->evt, 50);',
          '    while (InterlockedCompareExchange(&s->parked, 0, 0) > 0) {\n        xrx_state_guest_checkpoint();\n        WaitForSingleObject(s->evt, 50);\n    }')
s=replace(s,'    kernel_thunk_dispatch_body();\n    xbox_HostCodeLeave(1);',
          '    xrx_state_guest_location(slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE ? g_slot_ordinals[slot] : 0);\n    kernel_thunk_dispatch_body();\n    xbox_HostCodeLeave(1);')

# Registered guest threads use logical suspension on the parking stack. Keep
# the original path for startup handles not registered by their thread yet.
old='    SuspSlot *s = (tid && tid != GetCurrentThreadId() && !susp_raw()) ? susp_find(tid, 0) : NULL;'
s=replace(s,old,'''    ULONG state_previous;
    if(xrx_state_thread_suspend(rh,&state_previous)) {
        if(count_va)BRIDGE_MEM32(count_va)=state_previous;g_eax=0;return;
    }
'''+old)
old='    SuspSlot *s = (tid && !susp_raw()) ? susp_find(tid, 0) : NULL;'
s=replace(s,old,'''    ULONG state_previous;
    if(xrx_state_thread_resume(rh,&state_previous)) {
        if(count_va)BRIDGE_MEM32(count_va)=state_previous;g_eax=0;return;
    }
'''+old)

def vblank(body):
    body=replace(body,'    (void)unused;','    (void)unused;\n    xrx_state_device_join("vblank-ack");')
    body=replace(body,'    for (;;) {','    for (;;) {\n        xrx_state_device_checkpoint();')
    return body.replace('WaitForSingleObject(g_vbl_ack_event, INFINITE)','xrx_state_device_wait(g_vbl_ack_event, INFINITE)')
s=function(s,'static DWORD WINAPI kernel_vblank_ack_thread(LPVOID unused)',vblank)
s=function(s,'static void kernel_timer_loop_ns(void)',lambda b: replace(b,'    for (;;) {','    for (;;) {\n        xrx_state_device_checkpoint();'))
s=function(s,'static DWORD WINAPI kernel_timer_thread(LPVOID unused)',lambda b: replace(replace(b,
    '    if (!kernel_timer_paced()) {','    xrx_state_device_join("kernel-timer");\n    if (!kernel_timer_paced()) {'),
    '        for (;;) {','        for (;;) {\n            xrx_state_device_checkpoint();'))
appendix=Path(__file__).resolve().parent/'append'
s+=(appendix/'kernel_bridge_append.c').read_text(encoding='utf-8')
write('kernel_bridge_state.c',s)

s=(toolkit/'src/kernel/kernel_sync.c').read_text(encoding="utf-8")
s='#include "xrx_state_runtime.h"\n'+s
# These kernel_sync waits have no held object/guest-CPU locks. Add polling only
# here, not via a global Windows API replacement that could park inside locks.
s,n=re.subn(r'\bWaitForSingleObjectEx\(', 'xrx_state_wait_single(', s)
assert n==3
s,n=re.subn(r'\bWaitForMultipleObjectsEx\(', 'xrx_state_wait_multiple(', s)
assert n==2
write('kernel_sync_state.c',s)

s=(toolkit/'src/kernel/xbox_memory_layout.c').read_text(encoding='utf-8')
s='#include "xrx_state_runtime.h"\n'+s
def gpu(body):
    # After a load the restored DMA PUT is the snapshot's, but the segment
    # scanner's last_put is a function static from before the load; scanning
    # from it walks stale ring memory. Re-base it on the restored PUT.
    body=replace(body,'            if (!flip_held\n                    && (put != last_put',
         '            if (InterlockedExchange(&g_xrx_pb_resync, 0)) { last_put = put; last_put_ms = now_ms; }\n'
         '            if (!flip_held\n                    && (put != last_put')
    body=replace(body,'    volatile uint32_t *regs = (volatile uint32_t *)param;',
         '    volatile uint32_t *regs = (volatile uint32_t *)param;\n    xrx_state_device_join("gpu-executor");')
    body=replace(body,'    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {',
         '    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {\n        xrx_state_device_checkpoint();')
    return body.replace('    return 0;', '    xrx_state_device_part();\n    return 0;')
s=function(s,'static DWORD WINAPI nv2a_ack_thread(LPVOID param)',gpu)
s+=(appendix/'memory_layout_append.c').read_text(encoding='utf-8')
write('xbox_memory_layout_state.c',s)

s=(toolkit/'src/apu/apu_core.c').read_text(encoding='utf-8')
s='#include "xrx_state_runtime.h"\n'+s
def apu(body):
    body=replace(body,'    MCPXAPUState *d = MCPX_APU_DEVICE(arg);',
        '    MCPXAPUState *d = MCPX_APU_DEVICE(arg);\n    xrx_state_device_join("apu-frame");')
    body=replace(body,'            qemu_cond_wait(&d->cond, &d->lock);',
        '            qemu_cond_timedwait(&d->cond, &d->lock, 10);')
    body=replace(body,'            d->is_idle = false;\n            continue;',
        '            d->is_idle = false;\n            qemu_mutex_unlock(&d->lock);\n            xrx_state_device_checkpoint();\n            qemu_mutex_lock(&d->lock);\n            continue;')
    body=replace(body,'        qemu_mutex_unlock(&d->lock);\n        SwitchToThread();',
        '        qemu_mutex_unlock(&d->lock);\n        xrx_state_device_checkpoint();\n        SwitchToThread();')
    return replace(body,'    return NULL;','    xrx_state_device_part();\n    return NULL;')
s=function(s,'static void *mcpx_apu_frame_thread(void *arg)',apu)
s+=(appendix/'apu_core_append.c').read_text(encoding='utf-8')
write('apu_core_state.c',s)

s=(toolkit/'src/usb/ohci.c').read_text(encoding='utf-8')
s='#include "xrx_state_runtime.h"\n'+s
def ohci(body):
    body=replace(body,'    for (;;) {','    xrx_state_device_join("usb-ohci");\n    for (;;) {\n        xrx_state_device_checkpoint();')
    # Both early initialization failure and the final return are safe.
    return body.replace('return 0;','xrx_state_device_part();return 0;')
s=function(s,'static DWORD WINAPI ohci_thread(LPVOID unused)',ohci)
s+=(appendix/'ohci_append.c').read_text(encoding='utf-8')
write('ohci_state.c',s)

# A contended guest critical section blocks on the host; when its holder is
# parked for a save/load the pause never completes. Poll the same host lock
# with guest checkpoints instead (no-op on non-guest threads).
s=(toolkit/'src/kernel/kernel_rtl.c').read_text(encoding='utf-8')
s='#include "xrx_state_runtime.h"\n'+s
s=function(s,'VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)',lambda b: replace(b,
    '    xbox_GuestCpuBlocked();\n    EnterCriticalSection(cs);',
    '    xbox_GuestCpuBlocked();\n    {\n        unsigned spin = 0;\n        while (!TryEnterCriticalSection(cs)) {\n'
    '            xrx_state_guest_checkpoint();\n            if (++spin < 64) SwitchToThread(); else Sleep(1);\n        }\n    }'))
s+=(appendix/'kernel_rtl_append.c').read_text(encoding='utf-8')
write('kernel_rtl_state.c',s)
