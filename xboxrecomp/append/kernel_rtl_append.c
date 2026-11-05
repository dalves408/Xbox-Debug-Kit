
/* ---- save-state critical-section accessors (appended by generate_state_runtime.py) ----
 * Guests can be parked holding a guest lock, and a load moves each thread back to a
 * point where it holds different ones. The host locks must follow, or a lock stays
 * owned by a thread that no longer knows it holds it and the game deadlocks at its
 * next use. Only the owning thread can release a CRITICAL_SECTION through the API
 * and it is parked, so this writes the fields directly: debug build, same process,
 * no waiters (guest waits poll TryEnter here). Slots never move or free, so the
 * index is a stable ID. Callers hold every guest and device worker parked. */
typedef struct { uint32_t index, recursion; uint64_t owner; uintptr_t key; uint32_t site; long depth; unsigned long tid; } XrxCsEnt;
size_t xrx_csstate_max(void) { return sizeof(uint32_t) + sizeof(XrxCsEnt) * XBOX_CS_TABLE_SIZE; }
size_t xrx_csstate_capture(void *buf)
{
    uint32_t *count = buf; XrxCsEnt *e = (XrxCsEnt *)(count + 1); uint32_t i;
    *count = 0;
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++) {
        XBOX_CS_SLOT *s = &g_cs_table[i];
        /* Not RecursionCount alone: a title can have a lock with more leaves than enters, with or without loads,
         * which leaves a free lock with a negative count. Held means an owner. */
        if (!s->key || !s->cs.OwningThread || s->cs.RecursionCount <= 0) continue;
        e->index = i; e->recursion = (uint32_t)s->cs.RecursionCount; e->owner = (uint64_t)(uintptr_t)s->cs.OwningThread;
        e->key = s->key; e->site = s->owner_site; e->depth = s->depth; e->tid = s->owner_tid;
        e++; (*count)++;
    }
    return sizeof(uint32_t) + *count * sizeof(XrxCsEnt);
}
/* A waiter blocked in the host wait would be left behind by the field writes.
 * Windows keeps the inverted waiter count above bit 1 of LockCount. */
static int xrx_cs_waiters(const CRITICAL_SECTION *cs) { return (int)(((uint32_t)~cs->LockCount) >> 2); }
int xrx_csstate_validate(const void *buf, size_t n, char *err, size_t errn)
{
    const uint32_t *count = buf; const XrxCsEnt *e = (const XrxCsEnt *)(count + 1); uint32_t i;
    if (n < sizeof *count || *count > XBOX_CS_TABLE_SIZE || n != sizeof *count + *count * sizeof *e) { snprintf(err, errn, "critical-section section malformed"); return 0; }
    for (i = 0; i < *count; i++)
        if (e[i].index >= XBOX_CS_TABLE_SIZE || g_cs_table[e[i].index].key != e[i].key) { snprintf(err, errn, "critical section %u no longer matches the snapshot", e[i].index); return 0; }
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++)
        if (g_cs_table[i].key && xrx_cs_waiters(&g_cs_table[i].cs)) { snprintf(err, errn, "a thread is blocked on guest lock 0x%08X, cannot restore", (uint32_t)(g_cs_table[i].key - (uintptr_t)g_xbox_mem_offset)); return 0; }
    return 1;
}
void xrx_csstate_restore(const void *buf)
{
    const uint32_t *count = buf; const XrxCsEnt *e = (const XrxCsEnt *)(count + 1); uint32_t i, released = 0;
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++) {
        XBOX_CS_SLOT *s = &g_cs_table[i];
        if (!s->key) continue;
        if (s->cs.OwningThread) released++;
        s->cs.RecursionCount = 0; s->cs.OwningThread = NULL; s->cs.LockCount = -1;
        s->owner_site = 0; s->owner_tid = 0; s->depth = 0;
    }
    for (i = 0; i < *count; i++) {
        XBOX_CS_SLOT *s = &g_cs_table[e[i].index];
        s->cs.RecursionCount = (LONG)e[i].recursion; s->cs.OwningThread = (HANDLE)(uintptr_t)e[i].owner; s->cs.LockCount = -2;
        s->owner_site = e[i].site; s->owner_tid = e[i].tid; s->depth = e[i].depth;
    }
    fprintf(stderr, "[STATE] guest locks: %u were held, %u held at the snapshot\n", released, *count);
}
/* Printed by the heartbeat while no frames are coming: which guest locks are held, by whom, taken where. */
void xrx_cs_log_held(void)
{
    uint32_t i;
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++) {
        XBOX_CS_SLOT *s = &g_cs_table[i];
        if (s->key && s->cs.OwningThread)
            fprintf(stderr, "[STALL] guest lock %08X held by thread %lu x%ld, lockcount %ld, taken at guest %08X, %d waiting\n",
                    (uint32_t)(s->key - (uintptr_t)g_xbox_mem_offset), (unsigned long)(uintptr_t)s->cs.OwningThread,
                    (long)s->cs.RecursionCount, (long)s->cs.LockCount, s->owner_site, xrx_cs_waiters(&s->cs));
    }
}
