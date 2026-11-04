/* Experimental same-process, same-thread-topology native continuations.
 * Windows x64/MSVC only. Use only at explicit lock-free cooperative boundaries.
 * This restores native C stacks and adapter-selected TLS, NOT kernel/devices.
 * Do not use with CET user shadow stacks, AVX state, C++ unwinding or fibers. */
#ifndef XRDBG_NATIVE_H
#define XRDBG_NATIVE_H
#include <windows.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrDbgNativeGroup XrDbgNativeGroup;
typedef struct XrDbgNativeWorker XrDbgNativeWorker;
typedef struct XrDbgNativeConfig {
    size_t tls_size; /* 0..4096 bytes, fixed for this group */
    void (*capture_tls)(void *data); /* runs on the guest stack/thread */
    void (*restore_tls)(const void *data); /* same thread, on parking stack */
} XrDbgNativeConfig;
/* Error outputs have 256 bytes. Register/unregister on the worker itself.
 * Group lifetime must include every registered worker. Join threads before
 * destroy. A zero-worker group cannot be paused/captured. */
XrDbgNativeGroup *xrdbg_native_create(const XrDbgNativeConfig *config, char *error);
void xrdbg_native_destroy(XrDbgNativeGroup *group);
XrDbgNativeWorker *xrdbg_native_register(XrDbgNativeGroup *group, const char *name);
void xrdbg_native_unregister(XrDbgNativeWorker *worker);
/* Cheap no-op while no pause is requested. Must be called with no runtime,
 * allocator, GPU or device locks held; callers must also release guest CPU gates. */
void xrdbg_native_checkpoint(XrDbgNativeWorker *worker);
/* Adapter-defined diagnostic location, e.g. current guest kernel ordinal. */
void xrdbg_native_location(XrDbgNativeWorker *worker, unsigned location);
/* Coordinator thread only. begin freezes registered membership, waits for all
 * workers to move off their guest stacks, and unwinds on timeout. */
int xrdbg_native_begin(XrDbgNativeGroup *group, DWORD timeout_ms, char *error);
void xrdbg_native_end(XrDbgNativeGroup *group);
unsigned xrdbg_native_count(XrDbgNativeGroup *group);
/* Logical guest suspension; returns 0 for an unregistered target (adapter may
 * use its existing startup path). A resume before self-suspend leaves one token.
 * Other-thread suspension is cooperative, taking effect at its next checkpoint. */
int xrdbg_native_suspend(XrDbgNativeGroup *group,DWORD tid,ULONG *previous);
int xrdbg_native_resume(XrDbgNativeGroup *group,DWORD tid,ULONG *previous);
/* Call only between a successful begin/end on the owning coordinator.
 * capture returns a malloc-owned blob. validate writes nothing. restore first
 * validates every section and destination, then copies inactive native stacks.
 * Payloads contain process-local contexts/pointers: never load untrusted data. */
int xrdbg_native_capture(XrDbgNativeGroup *group, void **data, size_t *size, char *error);
int xrdbg_native_validate(XrDbgNativeGroup *group, const void *data, size_t size, char *error);
int xrdbg_native_restore(XrDbgNativeGroup *group, const void *data, size_t size, char *error);
#ifdef __cplusplus
}
#endif
#endif
