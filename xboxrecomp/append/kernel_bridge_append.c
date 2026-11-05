
/* ---- save-state kernel accessors (appended by generate_state_runtime.py) ----
 * Logical kernel-object state keyed by stable slot (bridge token index, shadow
 * guest VA). Restores are same-process: the slot must still name the same host
 * object, and only its signal state / count is reconciled. Callers hold every
 * guest and device worker parked. */
#include <winternl.h>
typedef LONG (NTAPI *XrxNtQuery)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { uint32_t kind, id; uint64_t host; int32_t type, state, aux, pad; char tname[16]; } XrxKObj;
typedef struct { uint32_t timer_va, dpc_va; int64_t rel_ms, period_ms; } XrxKTimer;
typedef struct { uint32_t magic, nobj, ntimers, dpc_pending; } XrxKHeader;
enum { XRX_KT_OTHER, XRX_KT_EVENT, XRX_KT_SEM, XRX_KT_MUTANT };
static XrxNtQuery s_q_obj, s_q_ev, s_q_sem, s_q_mut;
static void xrx_kq_init(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (s_q_obj || !nt) return;
    s_q_ev = (XrxNtQuery)GetProcAddress(nt, "NtQueryEvent");
    s_q_sem = (XrxNtQuery)GetProcAddress(nt, "NtQuerySemaphore");
    s_q_mut = (XrxNtQuery)GetProcAddress(nt, "NtQueryMutant");
    s_q_obj = (XrxNtQuery)GetProcAddress(nt, "NtQueryObject");
}
static int xrx_kq_type(HANDLE h, char *tname)
{
    union { PUBLIC_OBJECT_TYPE_INFORMATION t; uint8_t raw[512]; } u;
    ULONG got = 0;
    if (!s_q_obj || s_q_obj(h, 2, &u, sizeof u, &got) < 0 || !u.t.TypeName.Buffer) { strcpy(tname, "?"); return XRX_KT_OTHER; }
    snprintf(tname, 16, "%.*ls", (int)(u.t.TypeName.Length / 2), u.t.TypeName.Buffer);
    if (!wcsncmp(u.t.TypeName.Buffer, L"Event", u.t.TypeName.Length / 2)) return XRX_KT_EVENT;
    if (!wcsncmp(u.t.TypeName.Buffer, L"Semaphore", u.t.TypeName.Length / 2)) return XRX_KT_SEM;
    if (!wcsncmp(u.t.TypeName.Buffer, L"Mutant", u.t.TypeName.Length / 2)) return XRX_KT_MUTANT;
    return XRX_KT_OTHER;
}
static void xrx_kq_state(HANDLE h, XrxKObj *o)
{
    LONG v[2] = {0, 0};
    o->type = xrx_kq_type(h, o->tname); o->state = o->aux = 0;
    if (o->type == XRX_KT_EVENT && s_q_ev && s_q_ev(h, 0, v, sizeof v, NULL) >= 0) { o->aux = v[0]; o->state = v[1]; }
    else if (o->type == XRX_KT_SEM && s_q_sem && s_q_sem(h, 0, v, sizeof v, NULL) >= 0) { o->state = v[0]; o->aux = v[1]; }
    else if (o->type == XRX_KT_MUTANT && s_q_mut && s_q_mut(h, 0, v, sizeof v, NULL) >= 0) { o->state = v[0]; }
}
size_t xrx_kstate_max(void)
{
    return sizeof(XrxKHeader) + sizeof(XrxKObj) * (BRIDGE_HANDLE_MAX + KE_SHADOW_SIZE) + sizeof(XrxKTimer) * XBOX_MAX_TIMERS + sizeof(PendingDpc) * XBOX_MAX_PENDING_DPC;
}
int xrx_kstate_capture(void *buf, size_t cap, size_t *used, char *err, size_t errn)
{
    XrxKHeader *hd = buf; XrxKObj *o = (XrxKObj *)(hd + 1); XrxKTimer *t; uint32_t i;
    long long now = kernel_timer_clock_ms();
    if (cap < xrx_kstate_max()) { snprintf(err, errn, "kernel state buffer too small"); return 0; }
    xrx_kq_init();
    memset(hd, 0, sizeof *hd); hd->magic = 0x4B32564Du;
    hd->dpc_pending = (uint32_t)((g_dpc_tail - g_dpc_head + XBOX_MAX_PENDING_DPC) % XBOX_MAX_PENDING_DPC);
    handle_lock();
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++) if (s_handle_table[i]) {
        o->kind = 1; o->id = i; o->host = (uint64_t)(uintptr_t)s_handle_table[i]; o++; hd->nobj++;
    }
    handle_unlock();
    for (i = 0; i < KE_SHADOW_SIZE; i++) if (g_ke_shadow[i].in_use && g_ke_shadow[i].host_handle) {
        o->kind = 2; o->id = g_ke_shadow[i].guest_va; o->host = (uint64_t)(uintptr_t)g_ke_shadow[i].host_handle; o++; hd->nobj++;
    }
    for (o = (XrxKObj *)(hd + 1), i = 0; i < hd->nobj; i++) xrx_kq_state((HANDLE)(uintptr_t)o[i].host, &o[i]);
    t = (XrxKTimer *)(o + hd->nobj);
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++) if (g_timers[i].timer_va) {
        t->timer_va = g_timers[i].timer_va; t->dpc_va = g_timers[i].dpc_va;
        t->rel_ms = g_timers[i].due_ms - now; t->period_ms = g_timers[i].period_ms; t++; hd->ntimers++;
    }
    LeaveCriticalSection(&g_timer_lock);
    {   /* queued DPCs, in order (workers are parked, nothing inserts) */
        PendingDpc *d = (PendingDpc *)t; LONG k;
        for (k = g_dpc_head; k != g_dpc_tail; k = (k + 1) % XBOX_MAX_PENDING_DPC) *d++ = g_dpc_queue[k];
        *used = (uint8_t *)d - (uint8_t *)buf;
    }
    return 1;
}
static HANDLE xrx_kslot(const XrxKObj *o)
{
    uint32_t i;
    if (o->kind == 1) return o->id < BRIDGE_HANDLE_MAX ? s_handle_table[o->id] : NULL;
    for (i = 0; i < KE_SHADOW_SIZE; i++)
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == o->id) return g_ke_shadow[i].host_handle;
    return NULL;
}
int xrx_kstate_validate(const void *buf, size_t n, char *err, size_t errn)
{
    const XrxKHeader *hd = buf; const XrxKObj *o = (const XrxKObj *)(hd + 1); uint32_t i;
    if (n < sizeof *hd || hd->magic != 0x4B32564Du ||
        n != sizeof *hd + hd->nobj * sizeof *o + hd->ntimers * sizeof(XrxKTimer) + hd->dpc_pending * sizeof(PendingDpc) ||
        hd->ntimers > XBOX_MAX_TIMERS || hd->dpc_pending >= XBOX_MAX_PENDING_DPC) {
        snprintf(err, errn, "kernel state section malformed"); return 0;
    }
    /* A slot that now names another object (or none) is rebuilt on restore
     * only for plain sync objects; anything else (files, threads) refuses. */
    for (i = 0; i < hd->nobj; i++)
        if ((uint64_t)(uintptr_t)xrx_kslot(&o[i]) != o[i].host &&
            !(o[i].type == XRX_KT_EVENT || o[i].type == XRX_KT_SEM || (o[i].type == XRX_KT_MUTANT && o[i].state == 1))) {
            snprintf(err, errn, "kernel object %s %08X (%s) no longer names the same host object", o[i].kind == 1 ? "token" : "shadow", o[i].id, o[i].tname);
            return 0;
        }
    return 1;
}
int xrx_kstate_restore(const void *buf, size_t n, int *mutant_mismatch, char *err, size_t errn)
{
    const XrxKHeader *hd = buf; const XrxKObj *o = (const XrxKObj *)(hd + 1); const XrxKTimer *t; uint32_t i;
    long long now = kernel_timer_clock_ms();
    if (!xrx_kstate_validate(buf, n, err, errn)) return 0;
    static struct { uint64_t old; HANDLE made; } remap[512]; uint32_t nremap = 0, rebuilt = 0, j;
    *mutant_mismatch = 0;
    for (i = 0; i < hd->nobj; i++) {
        HANDLE h = (HANDLE)(uintptr_t)o[i].host; XrxKObj cur = o[i];
        if ((uint64_t)(uintptr_t)xrx_kslot(&o[i]) != o[i].host) {
            HANDLE made = NULL;
            for (j = 0; j < nremap; j++) if (remap[j].old == o[i].host) made = remap[j].made;
            if (!made) {
                if (o[i].type == XRX_KT_EVENT) made = CreateEventA(NULL, o[i].aux == 0, o[i].state != 0, NULL);
                else if (o[i].type == XRX_KT_SEM) made = CreateSemaphoreA(NULL, o[i].state, o[i].aux > 0 ? o[i].aux : 0x7FFFFFFF, NULL);
                else made = CreateMutexA(NULL, FALSE, NULL);
                if (!made) { snprintf(err, errn, "cannot rebuild kernel object %08X", o[i].id); return 0; }
                if (nremap < 512) { remap[nremap].old = o[i].host; remap[nremap++].made = made; }
                rebuilt++;
            }
            if (o[i].kind == 1) { handle_lock(); s_handle_table[o[i].id] = made; handle_unlock(); }
            else {
                for (j = 0; j < KE_SHADOW_SIZE; j++) if (g_ke_shadow[j].in_use && g_ke_shadow[j].guest_va == o[i].id) break;
                if (j == KE_SHADOW_SIZE) for (j = 0; j < KE_SHADOW_SIZE; j++) if (!g_ke_shadow[j].in_use) break;
                if (j == KE_SHADOW_SIZE) { snprintf(err, errn, "kernel shadow table full"); return 0; }
                g_ke_shadow[j].guest_va = o[i].id; g_ke_shadow[j].host_handle = made; g_ke_shadow[j].in_use = 1;
            }
            continue;
        }
        xrx_kq_state(h, &cur);
        if (o[i].type == XRX_KT_EVENT) { if (o[i].state) SetEvent(h); else ResetEvent(h); }
        else if (o[i].type == XRX_KT_SEM) {
            if (o[i].state > cur.state) ReleaseSemaphore(h, o[i].state - cur.state, NULL);
            else while (cur.state-- > o[i].state) WaitForSingleObject(h, 0);
        } else if (o[i].type == XRX_KT_MUTANT && cur.state != o[i].state) (*mutant_mismatch)++;
    }
    t = (const XrxKTimer *)(o + hd->nobj);
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++) g_timers[i].timer_va = 0;
    for (i = 0; i < hd->ntimers; i++) {
        g_timers[i].timer_va = t[i].timer_va; g_timers[i].dpc_va = t[i].dpc_va;
        g_timers[i].due_ms = now + t[i].rel_ms; g_timers[i].period_ms = t[i].period_ms;
    }
    LeaveCriticalSection(&g_timer_lock);
    memcpy(g_dpc_queue, t + hd->ntimers, hd->dpc_pending * sizeof(PendingDpc));
    g_dpc_head = 0; g_dpc_tail = (LONG)hd->dpc_pending;
    if (rebuilt) fprintf(stderr, "[STATE] rebuilt %u kernel sync objects whose slots changed since the snapshot\n", rebuilt);
    return 1;
}
