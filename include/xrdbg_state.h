/* Experimental host-assisted save states. Windows, debug builds only. */
#ifndef XRDBG_STATE_H
#define XRDBG_STATE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define XRDBG_STATE_ERROR_SIZE 256
#define XRDBG_STATE_MEMORY    (1u << 0)
#define XRDBG_STATE_EXECUTION (1u << 1)
#define XRDBG_STATE_KERNEL    (1u << 2)
#define XRDBG_STATE_GPU       (1u << 3)
#define XRDBG_STATE_AUDIO     (1u << 4)
#define XRDBG_STATE_FILES     (1u << 5)
#define XRDBG_STATE_CLOCK     (1u << 6)
#define XRDBG_STATE_INPUT     (1u << 7)
#define XRDBG_STATE_ALL       255u
typedef enum XrDbgStateOperation { XRDBG_STATE_SAVE = 1, XRDBG_STATE_LOAD = 2 } XrDbgStateOperation;
typedef enum XrDbgStateResult {
    XRDBG_STATE_OK, XRDBG_STATE_INVALID, XRDBG_STATE_BUSY, XRDBG_STATE_UNSUPPORTED,
    XRDBG_STATE_IO_ERROR, XRDBG_STATE_CORRUPT, XRDBG_STATE_INCOMPATIBLE,
    XRDBG_STATE_HOST_ERROR, XRDBG_STATE_ROLLED_BACK, XRDBG_STATE_POISONED
} XrDbgStateResult;
typedef struct XrDbgStateConfig {
    const char *game_id;       /* stable game/region identifier, 1..63 bytes */
    const char *build_id;      /* exact executable/runtime/schema identity, 1..95 bytes */
    uint32_t schema;           /* adapter payload schema, nonzero */
    size_t max_payload;       /* bounded allocation, at most 512 MiB */
    void *user;
    /* Required for a working adapter. Return a mask of restorable subsystems.
     * Any missing bit refuses the operation before pausing or opening files. */
    uint32_t (*capabilities)(void *user, XrDbgStateOperation op, char *error);
    /* Coordinated, bounded pause of ALL writers. On failure unwind partial
     * pauses yourself; the engine will not call resume. Never SuspendThread
     * arbitrarily while a thread can hold a CRT/driver/runtime lock. */
    int (*quiesce)(void *user, char *error);
    void (*resume)(void *user);
    /* Called while quiesced. Logical sections use offsets/IDs, never live Windows handles. Optional
     * native continuation sections contain validated same-process addresses.
     * Capture includes execution continuations, memory and every subsystem.
     * validate must not mutate; check all lengths/IDs before restore writes.
     * restore must also accept a capture made just before a failed restore. */
    int (*capture)(void *user, void *data, size_t capacity, size_t *used, char *error);
    int (*validate)(void *user, const void *data, size_t size, char *error);
    int (*restore)(void *user, const void *data, size_t size, char *error);
    /* Called instead of resume if restoring the rollback snapshot fails.
     * Keep the runtime paused and report/terminate; never continue corrupted. */
    void (*fatal)(void *user, const char *error);
} XrDbgStateConfig;
typedef struct XrDbgState XrDbgState;
/* Create/destroy outside an operation; config strings are copied. States are
 * deliberately restricted to this process instance and exact build/schema. */
XrDbgState *xrdbg_state_create(const XrDbgStateConfig *config);
void xrdbg_state_destroy(XrDbgState *state);
/* Synchronous: call from the adapter's coordinator, never an arbitrary WndProc
 * or device callback that quiesce would need to wait for. One slot per path.
 * Error output must have XRDBG_STATE_ERROR_SIZE bytes; may be NULL. */
XrDbgStateResult xrdbg_state_run(XrDbgState *state, XrDbgStateOperation operation,
                               const char *path, char *error);
const char *xrdbg_state_result_name(XrDbgStateResult result);
#ifdef __cplusplus
}
#endif
#endif
