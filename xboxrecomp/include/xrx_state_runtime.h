#ifndef XRX_STATE_RUNTIME_H
#define XRX_STATE_RUNTIME_H
#include <windows.h>
int xrx_state_runtime_init(char *error);
void xrx_state_runtime_shutdown(void);
void xrx_state_guest_join(void);
void xrx_state_guest_part(void);
void xrx_state_guest_checkpoint(void);
void xrx_state_guest_location(unsigned location);
unsigned xrx_state_runtime_count(void);
void xrx_state_device_join(const char *name);
void xrx_state_device_checkpoint(void);
void xrx_state_device_part(void);
DWORD xrx_state_device_wait(HANDLE event,DWORD milliseconds);
int xrx_state_thread_suspend(HANDLE handle,ULONG *previous);
int xrx_state_thread_resume(HANDLE handle,ULONG *previous);
/* Diagnostic staged guest/device capture; optional echo restores only the
 * just-captured native frames. No gameplay rewind or logical resource restore. */
int xrx_state_native_probe(char *error);
DWORD xrx_state_wait_single(HANDLE object,DWORD milliseconds,BOOL alertable);
DWORD xrx_state_wait_multiple(DWORD count,const HANDLE *objects,BOOL all,DWORD milliseconds,BOOL alertable);
#include <stdint.h>
/* Frame-boundary snapshot; pause parks every guest and device worker. */
int xrx_state_pause(char *error);
void xrx_state_unpause(void);
size_t xrx_state_snap_max(void);
int xrx_state_snap_capture(void *buf,size_t cap,size_t *used,uint64_t *ram_hash,char *error);
int xrx_state_snap_validate(const void *buf,size_t n,char *error);
int xrx_state_snap_restore(const void *buf,size_t n,uint64_t *ram_hash,char *error);
uint64_t xrx_state_hash(const void *data,size_t n);
uint64_t xrx_state_ram_hash(void);
/* Set after a load; the GPU executor re-bases its pushbuffer segment scan on the restored DMA PUT. */
extern volatile long g_xrx_pb_resync;
#endif

