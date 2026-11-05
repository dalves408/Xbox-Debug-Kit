
/* ---- save-state APU accessors (appended by generate_state_runtime.py) ----
 * The APU's guest-visible state outside guest memory: registers, sequencer count,
 * interrupt line and per-voice runtime data. Voice descriptors and stream packets
 * live in contiguous RAM, so without this they roll back while the registers stay
 * in the future, and the title never gets the completions its streams wait on.
 * Left live on purpose: locks and threads, the host output queue, pacing clocks.
 * Callers hold every guest and device worker parked. */
#define XRX_ASTATE_FIELDS(X, d) \
    X((d)->regs) X((d)->sample_count) X((d)->gp.regs) X((d)->ep.regs) \
    X((d)->vp.ssl_base_page) X((d)->vp.ssl) X((d)->vp.hrtf_headroom) X((d)->vp.hrtf_submix) \
    X((d)->vp.submix_headroom) X((d)->vp.voice_locked) X((d)->vp.hrtf) \
    X((d)->vp.inbuf_sge_handle) X((d)->vp.outbuf_sge_handle)
size_t xrx_astate_size(void)
{
    size_t n = sizeof(LONG) + sizeof(((MCPXAPUState *)0)->vp.filters);
#define X(v) n += sizeof(v);
    XRX_ASTATE_FIELDS(X, (MCPXAPUState *)0)
#undef X
    return n;
}
void xrx_astate_capture(void *buf)
{
    uint8_t *p = buf; MCPXAPUState *d = g_state; LONG line = s_irq_line;
    memset(buf, 0, xrx_astate_size());
    if (!d) return;
    memcpy(p, &line, sizeof line); p += sizeof line;
#define X(v) memcpy(p, (const void *)&(v), sizeof(v)); p += sizeof(v);
    XRX_ASTATE_FIELDS(X, d)
#undef X
    memcpy(p, d->vp.filters, sizeof d->vp.filters);
}
void xrx_astate_restore(const void *buf)
{
    const uint8_t *p = buf; MCPXAPUState *d = g_state; LONG line; int i;
    if (!d) return;
    qemu_mutex_lock(&d->lock);
    memcpy(&line, p, sizeof line); p += sizeof line;
    InterlockedExchange(&s_irq_line, line);
#define X(v) memcpy((void *)&(v), p, sizeof(v)); p += sizeof(v);
    XRX_ASTATE_FIELDS(X, d)
#undef X
    /* Everything in a voice filter is plain data except its host resampler handle. */
    for (i = 0; i < MCPX_HW_MAX_VOICES; i++) {
        SRC_STATE *keep = d->vp.filters[i].resampler;
        memcpy(&d->vp.filters[i], p + i * sizeof d->vp.filters[0], sizeof d->vp.filters[0]);
        d->vp.filters[i].resampler = keep;
    }
    qemu_mutex_unlock(&d->lock);
}
/* Printed by the heartbeat while no frames are coming. */
void xrx_apu_log_state(void)
{
    MCPXAPUState *d = g_state;
    if (d) fprintf(stderr, "[STALL] APU ISTS=%08X IEN=%08X FECTL=%08X SECTL=%08X line=%ld idle=%d\n", d->regs[NV_PAPU_ISTS], d->regs[NV_PAPU_IEN],
                   d->regs[NV_PAPU_FECTL], d->regs[NV_PAPU_SECTL], (long)s_irq_line, (int)d->is_idle);
}
