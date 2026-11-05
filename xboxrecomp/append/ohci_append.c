
/* ---- save-state USB accessors (appended by generate_state_runtime.py) ----
 * The host controllers' registers and each pad's control pipe. The endpoint and
 * transfer descriptors live in guest RAM, so without this they roll back while the
 * controller's done queue, frame number and interrupt status stay in the future,
 * and the title stops seeing pad reports. Callers hold every worker parked. */
#define XRX_USTATE_FIELDS(X) \
    X(s_hc) X(s_device_hc) X(g_setup_pending_) X(g_setup_) X(g_ctrl_buf_) X(g_ctrl_len_) X(g_ctrl_sent_) X(s_pad_plugged)
size_t xrx_ustate_size(void)
{
    size_t n = 0;
#define X(v) n += sizeof(v);
    XRX_USTATE_FIELDS(X)
#undef X
    return n;
}
void xrx_ustate_capture(void *buf)
{
    uint8_t *p = buf;
#define X(v) memcpy(p, (const void *)&(v), sizeof(v)); p += sizeof(v);
    XRX_USTATE_FIELDS(X)
#undef X
}
void xrx_ustate_restore(const void *buf)
{
    const uint8_t *p = buf;
#define X(v) memcpy((void *)&(v), p, sizeof(v)); p += sizeof(v);
    XRX_USTATE_FIELDS(X)
#undef X
}
