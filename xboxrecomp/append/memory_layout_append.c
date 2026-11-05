
/* ---- save-state accessors (appended by generate_state_runtime.py) ----
 * Callers must hold every guest and device worker parked. */
typedef struct { const char *name; void *host; size_t size; } XrxMemRegion;
int xrx_mstate_regions(XrxMemRegion *out, int max)
{
    int n = 0;
    if (max < 4) return 0;
    if (g_memory_base) { out[n].name = "ram"; out[n].host = g_memory_base; out[n].size = g_xbox_total_ram; n++; }
    if (g_contig_memory) { out[n].name = "contig"; out[n].host = g_contig_memory; out[n].size = XBOX_CONTIG_SIZE; n++; }
    if (g_nv2a_memory) { out[n].name = "nv2a"; out[n].host = g_nv2a_memory; out[n].size = XBOX_NV2A_SIZE; n++; }
    return n;
}
#define XRX_MSTATE_FIELDS(X) \
    X(g_heap_next) X(g_heap_alloc_count) X(g_heap_blocks) X(g_heap_block_count) X(g_reserve_next) \
    X(g_thread_stacks_used) X(g_contig_used) X(g_contig_len) X(g_contig_pages_in_use) X(g_contig_high) \
    X(g_contig_first_free) X(g_contig_allocs) X(g_contig_frees) X(g_contig_peak_pages) X(g_phys_ram_named)
size_t xrx_mstate_meta_size(void)
{
    size_t n = 0;
#define X(v) n += sizeof(v);
    XRX_MSTATE_FIELDS(X)
#undef X
    return n;
}
void xrx_mstate_meta_capture(void *buf)
{
    uint8_t *p = buf;
#define X(v) memcpy(p, (const void *)&(v), sizeof(v)); p += sizeof(v);
    XRX_MSTATE_FIELDS(X)
#undef X
}
void xrx_mstate_meta_restore(const void *buf)
{
    const uint8_t *p = buf;
#define X(v) memcpy((void *)&(v), p, sizeof(v)); p += sizeof(v);
    XRX_MSTATE_FIELDS(X)
#undef X
}

