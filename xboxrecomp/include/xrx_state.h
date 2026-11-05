#ifndef XRX_STATE_H
#define XRX_STATE_H
/* Experimental debug-build adapter. Keyboard requests are queued from the
 * render thread; state work belongs on a dedicated runtime coordinator. */
typedef struct {
    const char *game_id;        /* stable game + region, e.g. "mygame-xbox-ntsc-u" */
    const char *build_id;       /* exact build, e.g. "1.0 " __DATE__ " " __TIME__ */
    /* Optional, on the coordinator with every worker parked, after a load: drop whatever
     * the game's renderer keeps outside guest memory (texture caches, pending readbacks). */
    void (*after_load)(void);
} XrxStateConfig;
void xrx_state_init(const XrxStateConfig *config);
void xrx_state_keys(int focused);
void xrx_state_shutdown(void);
#include <stdint.h>
void xrx_state_on_flip(uint32_t va,uint32_t w,uint32_t h,uint32_t pitch,uint32_t bpp);
/* From the debug kit's heartbeat, with the presented-frame count: logs [STALL] lines (guest
 * locks held, APU interrupt state) while no frames are coming. XRDBG_STATE_LOCKLOG=1: always. */
void xrx_state_heartbeat(uint32_t flips);
#endif
