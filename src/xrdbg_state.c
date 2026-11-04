#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrdbg_state.h"

/* Fixed-layout, little-endian Windows file envelope. Payloads remain adapter
 * owned. Integrity checks detect corruption, not maliciously crafted files. */
typedef struct StateHeader {
    char magic[8];
    uint32_t version, header_size, schema, reserved;
    uint64_t payload_size, checksum;
    char game[64], build[96], session[48];
} StateHeader;
typedef char state_header_size_check[sizeof(StateHeader) == 248 ? 1 : -1];
struct XrDbgState {
    XrDbgStateConfig cfg;
    StateHeader identity;
    volatile LONG busy;
    int poisoned;
};
static void message(char *out, const char *text)
{
    if (out) snprintf(out, XRDBG_STATE_ERROR_SIZE, "%s", text);
}
static uint64_t hash_bytes(const void *data, size_t size, uint64_t hash)
{
    const unsigned char *p = (const unsigned char *)data;
    while (size--) { hash ^= *p++; hash *= UINT64_C(1099511628211); }
    return hash;
}
static uint64_t checksum(StateHeader header, const void *payload)
{
    header.checksum = 0;
    return hash_bytes(payload, (size_t)header.payload_size,
        hash_bytes(&header, sizeof header, UINT64_C(14695981039346656037)));
}
XrDbgState *xrdbg_state_create(const XrDbgStateConfig *cfg)
{
    XrDbgState *s;
    FILETIME created, exited, kernel, user;
    if (!cfg || !cfg->game_id || !*cfg->game_id || strlen(cfg->game_id) >= 64 ||
        !cfg->build_id || !*cfg->build_id || strlen(cfg->build_id) >= 96 ||
        !cfg->schema || !cfg->max_payload || cfg->max_payload > 512u*1024u*1024u ||
        !cfg->capabilities) return NULL;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return NULL;
    s = (XrDbgState *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg;
    memcpy(s->identity.magic, "XRSTATE\0", 8);
    s->identity.version = 1; s->identity.header_size = sizeof(StateHeader);
    s->identity.schema = cfg->schema;
    strcpy(s->identity.game, cfg->game_id); strcpy(s->identity.build, cfg->build_id);
    snprintf(s->identity.session, sizeof s->identity.session, "%08lX-%08lX%08lX",
             GetCurrentProcessId(), created.dwHighDateTime, created.dwLowDateTime);
    s->cfg.game_id = s->identity.game; s->cfg.build_id = s->identity.build;
    return s;
}
void xrdbg_state_destroy(XrDbgState *s) { free(s); }

static int transfer(HANDLE file, void *data, size_t bytes, int writing)
{
    unsigned char *p = (unsigned char *)data;
    while (bytes) {
        DWORD done = 0, part = bytes > 1024u*1024u ? 1024u*1024u : (DWORD)bytes;
        BOOL ok = writing ? WriteFile(file,p,part,&done,NULL) : ReadFile(file,p,part,&done,NULL);
        if (!ok || !done) return 0;
        p += done; bytes -= done;
    }
    return 1;
}
static XrDbgStateResult write_state(const char *path, StateHeader *header, void *data, char *error)
{
    /* CREATE_NEW + a unique sibling: failure never truncates an existing slot. */
    static volatile LONG sequence;
    char temporary[MAX_PATH];
    HANDLE file;
    int ok, n = snprintf(temporary,sizeof temporary,"%s.%lu.%ld.tmp",path,
                        GetCurrentProcessId(),InterlockedIncrement(&sequence));
    if (n < 0 || n >= sizeof temporary) { message(error,"State path is too long."); return XRDBG_STATE_INVALID; }
    file = CreateFileA(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) { message(error,"Cannot create the temporary state file."); return XRDBG_STATE_IO_ERROR; }
    header->checksum = checksum(*header,data);
    ok = transfer(file,header,sizeof *header,1) && transfer(file,data,(size_t)header->payload_size,1) && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = 0;
    if (ok) ok = MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(temporary);
        message(error,"Could not publish the state file; the previous slot was retained.");
        return XRDBG_STATE_IO_ERROR;
    }
    return XRDBG_STATE_OK;
}
static XrDbgStateResult read_state(XrDbgState *s, const char *path, void **data, size_t *size, char *error)
{
    HANDLE file = CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    LARGE_INTEGER length;
    StateHeader h;
    XrDbgStateResult result = XRDBG_STATE_CORRUPT;
    if (file == INVALID_HANDLE_VALUE) { message(error,"Cannot open the state file."); return XRDBG_STATE_IO_ERROR; }
    message(error,"State file is truncated, oversized or corrupt.");
    if (!GetFileSizeEx(file,&length) || !transfer(file,&h,sizeof h,0)) goto done;
    if (memcmp(h.magic,"XRSTATE\0",8) || h.version != 1 || h.header_size != sizeof h ||
        h.reserved || !h.payload_size || h.payload_size > s->cfg.max_payload ||
        (uint64_t)length.QuadPart != sizeof h + h.payload_size) goto done;
    *data = malloc((size_t)h.payload_size);
    if (!*data) { result = XRDBG_STATE_HOST_ERROR; message(error,"Cannot allocate the state payload."); goto done; }
    if (!transfer(file,*data,(size_t)h.payload_size,0) || h.checksum != checksum(h,*data)) goto done;
    if (memcmp(h.game,s->identity.game,sizeof h.game) || memcmp(h.build,s->identity.build,sizeof h.build) ||
        memcmp(h.session,s->identity.session,sizeof h.session) || h.schema != s->cfg.schema) {
        result = XRDBG_STATE_INCOMPATIBLE;
        message(error,"State belongs to another game, build, schema or process session.");
        goto done;
    }
    *size = (size_t)h.payload_size; result = XRDBG_STATE_OK; message(error,"");
done:
    CloseHandle(file);
    if (result != XRDBG_STATE_OK) { free(*data); *data = NULL; }
    return result;
}
XrDbgStateResult xrdbg_state_run(XrDbgState *s, XrDbgStateOperation op, const char *path, char *out)
{
    char error[XRDBG_STATE_ERROR_SIZE] = "", rollback_error[XRDBG_STATE_ERROR_SIZE] = "";
    void *data = NULL, *previous = NULL;
    size_t used = 0, previous_used = 0;
    int paused = 0;
    uint32_t available;
    StateHeader header;
    XrDbgStateResult result = XRDBG_STATE_HOST_ERROR;
    message(out,"");
    if (!s || !path || !*path || (op != XRDBG_STATE_SAVE && op != XRDBG_STATE_LOAD)) {
        message(out,"Invalid save-state request."); return XRDBG_STATE_INVALID;
    }
    if (InterlockedCompareExchange(&s->busy,1,0)) { message(out,"A state operation is already running."); return XRDBG_STATE_BUSY; }
    if (s->poisoned) { result = XRDBG_STATE_POISONED; message(error,"Runtime is held paused after rollback failure."); goto done; }
    available = s->cfg.capabilities(s->cfg.user,op,error);
    error[sizeof error-1] = 0;
    if ((available & XRDBG_STATE_ALL) != XRDBG_STATE_ALL || !s->cfg.quiesce || !s->cfg.resume ||
        !s->cfg.capture || !s->cfg.validate || !s->cfg.restore || !s->cfg.fatal) {
        result = XRDBG_STATE_UNSUPPORTED;
        if (!*error) message(error,"Adapter does not yet restore every required subsystem.");
        goto done;
    }
    message(error,"");
    if (op == XRDBG_STATE_LOAD) {
        result = read_state(s,path,&data,&used,error);
        if (result != XRDBG_STATE_OK) goto done;
        previous = malloc(s->cfg.max_payload);
        if (!previous) { result = XRDBG_STATE_HOST_ERROR; message(error,"Cannot allocate rollback storage."); goto done; }
    } else {
        data = malloc(s->cfg.max_payload);
        if (!data) { message(error,"Cannot allocate capture storage."); goto done; }
    }
    result = XRDBG_STATE_HOST_ERROR;
    if (!s->cfg.quiesce(s->cfg.user,error)) goto done;
    paused = 1;
    if (op == XRDBG_STATE_SAVE) {
        if (!s->cfg.capture(s->cfg.user,data,s->cfg.max_payload,&used,error) || !used || used > s->cfg.max_payload) goto done;
        if (!s->cfg.validate(s->cfg.user,data,used,error)) goto done;
        s->cfg.resume(s->cfg.user); paused = 0;
        header = s->identity; header.payload_size = used;
        result = write_state(path,&header,data,error);
    } else {
        if (!s->cfg.validate(s->cfg.user,data,used,error)) goto done;
        if (!s->cfg.capture(s->cfg.user,previous,s->cfg.max_payload,&previous_used,error) ||
            !previous_used || previous_used > s->cfg.max_payload ||
            !s->cfg.validate(s->cfg.user,previous,previous_used,error)) goto done;
        if (!s->cfg.restore(s->cfg.user,data,used,error)) {
            if (!s->cfg.restore(s->cfg.user,previous,previous_used,rollback_error)) {
                s->poisoned = 1; paused = 0; result = XRDBG_STATE_POISONED;
                message(error,"State restore and rollback failed; runtime must remain paused.");
                s->cfg.fatal(s->cfg.user,error);
            } else result = XRDBG_STATE_ROLLED_BACK;
        } else result = XRDBG_STATE_OK;
    }
done:
    if (paused) s->cfg.resume(s->cfg.user);
    if (result == XRDBG_STATE_OK) message(error,"");
    else if (!*error) message(error,"Host save-state callback failed.");
    error[sizeof error-1] = 0;
    message(out,error); free(data); free(previous);
    InterlockedExchange(&s->busy,0);
    return result;
}
const char *xrdbg_state_result_name(XrDbgStateResult result)
{
    static const char *names[] = {"ok","invalid","busy","unsupported","io-error","corrupt","incompatible",
                                 "host-error","rolled-back","poisoned"};
    return (unsigned)result < sizeof names/sizeof names[0] ? names[result] : "unknown";
}
