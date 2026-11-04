#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "xrdbg_native.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_M_X64) || !defined(_MSC_VER)
#error Experimental native continuations require MSVC Windows x64.
#endif
#define NATIVE_MAX_WORKERS 64
#define PARK_BYTES (1024u*1024u)
#define MAX_STACK_BYTES (16u*1024u*1024u)
struct XrDbgNativeWorker {
    XrDbgNativeGroup *group;
    DWORD tid;
    uint64_t id;
    char name[48];
    void *parking, *original_base, *original_limit;
    CONTEXT current, selected;
    unsigned char tls[4096];
    volatile LONG returning;
    int active, parked, loaded;
    volatile LONG location;
    ULONG suspend_count, resume_token;
};
struct XrDbgNativeGroup {
    XrDbgNativeConfig cfg;
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    volatile LONG requested;
    DWORD owner;
    uint64_t epoch, topology, next_id, identity;
    XrDbgNativeWorker workers[NATIVE_MAX_WORKERS];
};
typedef struct NativeHeader { uint64_t magic, group, topology; uint32_t count, tls_size; } NativeHeader;
typedef struct NativeEntry {
    uint64_t id, stack_base, stack_pointer, stack_bytes;
    DWORD tid, reserved;
    ULONG suspend_count, resume_token;
    CONTEXT context;
} NativeEntry;
__declspec(noreturn) void xrdbg_native_stack_enter(XrDbgNativeWorker *, void *, void (*)(XrDbgNativeWorker *));
static volatile LONG64 next_group_identity;
static void error_text(char *error, const char *text) { if(error)snprintf(error,256,"%s",text); }
static NT_TIB *current_tib(void) { return (NT_TIB *)NtCurrentTeb(); }
static int all_parked(XrDbgNativeGroup *g)
{
    unsigned n=0;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++) if(g->workers[i].active) {
        n++; if(!g->workers[i].parked)return 0;
    }
    return n!=0;
}
static int any_parked(XrDbgNativeGroup *g)
{
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].parked&&!g->workers[i].suspend_count)return 1;
    return 0;
}
XrDbgNativeGroup *xrdbg_native_create(const XrDbgNativeConfig *cfg,char *error)
{
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy={0};
    XrDbgNativeGroup *g;
    error_text(error,"");
    if(!cfg || cfg->tls_size>4096 || (cfg->tls_size && (!cfg->capture_tls||!cfg->restore_tls))) {
        error_text(error,"Invalid native continuation configuration.");return NULL;
    }
    if(IsThreadAFiber()) {error_text(error,"Fiber-hosted threads are unsupported.");return NULL;}
    if(GetProcessMitigationPolicy(GetCurrentProcess(),ProcessUserShadowStackPolicy,&policy,sizeof policy) && policy.EnableUserShadowStack) {
        error_text(error,"Native continuations are unavailable with CET user shadow stacks enabled.");return NULL;
    }
    g=(XrDbgNativeGroup *)_aligned_malloc(sizeof *g,16);
    if(!g){error_text(error,"Cannot allocate continuation group.");return NULL;}
    memset(g,0,sizeof *g);g->cfg=*cfg;InitializeSRWLock(&g->lock);InitializeConditionVariable(&g->changed);
    g->identity=(uint64_t)InterlockedIncrement64(&next_group_identity);
    return g;
}
void xrdbg_native_destroy(XrDbgNativeGroup *g)
{
    if(!g)return;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++) {
        /* A caller must stop/join workers first. Fail closed on misuse. */
        if(g->workers[i].active)return;
    }
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].parking)VirtualFree(g->workers[i].parking,0,MEM_RELEASE);
    _aligned_free(g);
}
XrDbgNativeWorker *xrdbg_native_register(XrDbgNativeGroup *g,const char *name)
{
    XrDbgNativeWorker *s=NULL;
    void *parking;
    if(!g||IsThreadAFiber())return NULL;
    parking=VirtualAlloc(NULL,PARK_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!parking)return NULL;
    /* First page is a hard bound; no stack-growth/exception unwinding on this stack. */
    {DWORD old; if(!VirtualProtect(parking,4096,PAGE_NOACCESS,&old)){VirtualFree(parking,0,MEM_RELEASE);return NULL;}}
    AcquireSRWLockExclusive(&g->lock);
    while(g->owner)SleepConditionVariableSRW(&g->changed,&g->lock,INFINITE,0);
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active&&g->workers[i].tid==GetCurrentThreadId()) {
        ReleaseSRWLockExclusive(&g->lock);VirtualFree(parking,0,MEM_RELEASE);return NULL;
    }
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(!g->workers[i].active) {
        s=&g->workers[i];
        if(s->parking)VirtualFree(s->parking,0,MEM_RELEASE);
        memset(s,0,sizeof *s);s->group=g;s->parking=parking;s->tid=GetCurrentThreadId();
        s->id=++g->next_id;s->active=1;g->topology++;
        snprintf(s->name,sizeof s->name,"%s",name?name:"worker");break;
    }
    ReleaseSRWLockExclusive(&g->lock);
    if(!s)VirtualFree(parking,0,MEM_RELEASE);
    return s;
}
void xrdbg_native_unregister(XrDbgNativeWorker *s)
{
    XrDbgNativeGroup *g;
    if(!s||s->tid!=GetCurrentThreadId())return;
    xrdbg_native_checkpoint(s);g=s->group;
    AcquireSRWLockExclusive(&g->lock);s->active=0;g->topology++;
    WakeAllConditionVariable(&g->changed);ReleaseSRWLockExclusive(&g->lock);
}
static __declspec(noreturn) void park_worker(XrDbgNativeWorker *s)
{
    XrDbgNativeGroup *g=s->group;
    NT_TIB *tib=current_tib();
    AcquireSRWLockExclusive(&g->lock);
    if(g->requested||s->suspend_count) {
        s->parked=1;WakeAllConditionVariable(&g->changed);
        while(g->requested||s->suspend_count)SleepConditionVariableSRW(&g->changed,&g->lock,INFINITE,0);
        s->parked=0;WakeAllConditionVariable(&g->changed);
    }
    ReleaseSRWLockExclusive(&g->lock);
    if(s->loaded) {if(g->cfg.tls_size)g->cfg.restore_tls(s->tls);s->loaded=0;}
    tib->StackBase=s->original_base;tib->StackLimit=s->original_limit;
    InterlockedExchange(&s->returning,1);
    RtlRestoreContext(&s->selected,NULL);
    __assume(0);
}
__declspec(noinline) void xrdbg_native_checkpoint(XrDbgNativeWorker *s)
{
    NT_TIB *tib;
    if(!s)return;
    if(!InterlockedCompareExchange(&s->group->requested,0,0)&&!InterlockedCompareExchange((volatile LONG *)&s->suspend_count,0,0))return;
    if(s->tid!=GetCurrentThreadId())return;
    tib=current_tib();s->original_base=tib->StackBase;s->original_limit=tib->StackLimit;
    if(s->group->cfg.tls_size)s->group->cfg.capture_tls(s->tls);
    RtlCaptureContext(&s->current);
    /* This load is intentionally outside the captured stack. RtlRestoreContext
     * returns here after either normal resume or restoring an older C call chain. */
    if(InterlockedExchange(&s->returning,0))return;
    s->selected=s->current;
    tib->StackBase=(char *)s->parking+PARK_BYTES;tib->StackLimit=(char *)s->parking+4096;
    xrdbg_native_stack_enter(s,tib->StackBase,park_worker);
}
void xrdbg_native_location(XrDbgNativeWorker *s,unsigned location) {if(s)InterlockedExchange(&s->location,(LONG)location);}
int xrdbg_native_begin(XrDbgNativeGroup *g,DWORD timeout,char *error)
{
    ULONGLONG deadline=GetTickCount64()+timeout;
    if(!g){error_text(error,"No continuation group.");return 0;}
    AcquireSRWLockExclusive(&g->lock);
    if(g->owner) {ReleaseSRWLockExclusive(&g->lock);error_text(error,"Continuation group is busy.");return 0;}
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active&&g->workers[i].tid==GetCurrentThreadId()) {
        ReleaseSRWLockExclusive(&g->lock);error_text(error,"Coordinator cannot pause itself.");return 0;
    }
    g->owner=GetCurrentThreadId();g->epoch++;InterlockedExchange(&g->requested,1);
    while(!all_parked(g)) {
        ULONGLONG now=GetTickCount64();
        if(now>=deadline) {
            if(error) {
                size_t used=0;
                used+=(size_t)snprintf(error,256,"Native pause timeout; unparked tid:location:");
                for(unsigned i=0;i<NATIVE_MAX_WORKERS&&used<240;i++)if(g->workers[i].active&&!g->workers[i].parked)
                    used+=(size_t)snprintf(error+used,256-used," %lu:%ld",g->workers[i].tid,g->workers[i].location);
            }
            InterlockedExchange(&g->requested,0);g->owner=0;WakeAllConditionVariable(&g->changed);
            ReleaseSRWLockExclusive(&g->lock);return 0;
        }
        SleepConditionVariableSRW(&g->changed,&g->lock,(DWORD)(deadline-now),0);
    }
    ReleaseSRWLockExclusive(&g->lock);error_text(error,"");return 1;
}
void xrdbg_native_end(XrDbgNativeGroup *g)
{
    if(!g)return;
    AcquireSRWLockExclusive(&g->lock);
    if(g->owner==GetCurrentThreadId()) {
        InterlockedExchange(&g->requested,0);WakeAllConditionVariable(&g->changed);
        while(any_parked(g))SleepConditionVariableSRW(&g->changed,&g->lock,INFINITE,0);
        g->owner=0;WakeAllConditionVariable(&g->changed);
    }
    ReleaseSRWLockExclusive(&g->lock);
}
unsigned xrdbg_native_count(XrDbgNativeGroup *g)
{
    unsigned count=0;if(!g)return 0;
    AcquireSRWLockShared(&g->lock);
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)count+=g->workers[i].active!=0;
    ReleaseSRWLockShared(&g->lock);return count;
}
static int owned(XrDbgNativeGroup *g,char *error)
{
    if(!g||g->owner!=GetCurrentThreadId()||!g->requested||!all_parked(g)) {
        error_text(error,"Capture/restore requires an owned cooperative pause.");return 0;
    }return 1;
}
static int writable_span(uint64_t start,uint64_t bytes)
{
    uint64_t end=start+bytes;
    while(start<end) {
        MEMORY_BASIC_INFORMATION m;
        if(!VirtualQuery((void *)(uintptr_t)start,&m,sizeof m)||m.State!=MEM_COMMIT||
           (m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||!(m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY)))return 0;
        start=(uint64_t)(uintptr_t)m.BaseAddress+m.RegionSize;
    }return 1;
}
int xrdbg_native_capture(XrDbgNativeGroup *g,void **data,size_t *size,char *error)
{
    size_t total=sizeof(NativeHeader),offset;
    NativeHeader h;
    unsigned count=0;
    if(!data||!size)return 0;*data=NULL;*size=0;
    if(!owned(g,error))return 0;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active) {
        XrDbgNativeWorker *s=&g->workers[i];uint64_t top=(uint64_t)(uintptr_t)s->original_base;
        if(s->current.Rsp>=top||top-s->current.Rsp>MAX_STACK_BYTES||!writable_span(s->current.Rsp,top-s->current.Rsp)) {
            error_text(error,"Unsupported native stack span.");return 0;
        }
        total+=sizeof(NativeEntry)+(size_t)(top-s->current.Rsp)+g->cfg.tls_size;count++;
    }
    *data=malloc(total);if(!*data){error_text(error,"Cannot allocate native snapshot.");return 0;}
    memset(&h,0,sizeof h);h.magic=UINT64_C(0x31564e4742445258);h.group=g->identity;
    h.topology=g->topology;h.count=count;h.tls_size=(uint32_t)g->cfg.tls_size;
    memcpy(*data,&h,sizeof h);offset=sizeof h;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active) {
        XrDbgNativeWorker *s=&g->workers[i];NativeEntry e;
        memset(&e,0,sizeof e);e.id=s->id;e.tid=s->tid;e.stack_base=(uint64_t)(uintptr_t)s->original_base;
        e.stack_pointer=s->current.Rsp;e.stack_bytes=e.stack_base-e.stack_pointer;e.context=s->current;e.suspend_count=s->suspend_count;e.resume_token=s->resume_token;
        memcpy((char *)*data+offset,&e,sizeof e);offset+=sizeof e;
        memcpy((char *)*data+offset,(void *)(uintptr_t)e.stack_pointer,(size_t)e.stack_bytes);offset+=(size_t)e.stack_bytes;
        memcpy((char *)*data+offset,s->tls,g->cfg.tls_size);offset+=g->cfg.tls_size;
    }
    *size=total;return 1;
}
int xrdbg_native_validate(XrDbgNativeGroup *g,const void *data,size_t size,char *error)
{
    NativeHeader h;size_t offset=sizeof h;unsigned count=0;
    if(!owned(g,error))return 0;
    error_text(error,"Invalid native snapshot or changed thread topology.");
    if(!data||size<sizeof h)return 0;memcpy(&h,data,sizeof h);
    if(h.magic!=UINT64_C(0x31564e4742445258)||h.group!=g->identity||h.topology!=g->topology||h.tls_size!=g->cfg.tls_size)return 0;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active) {
        XrDbgNativeWorker *s=&g->workers[i];NativeEntry e;
        if(size-offset<sizeof e)return 0;memcpy(&e,(char *)data+offset,sizeof e);offset+=sizeof e;
        if(e.id!=s->id||e.tid!=s->tid||e.reserved||e.suspend_count>127||e.resume_token>1||e.stack_base!=(uint64_t)(uintptr_t)s->original_base||
           !e.stack_bytes||e.stack_bytes>MAX_STACK_BYTES||e.stack_pointer>=e.stack_base||
           e.stack_base-e.stack_pointer!=e.stack_bytes||e.context.Rsp!=e.stack_pointer||
           e.context.Rip!=s->current.Rip||e.context.ContextFlags!=s->current.ContextFlags||
           e.stack_bytes>size-offset||!writable_span(e.stack_pointer,e.stack_bytes))return 0;
        offset+=(size_t)e.stack_bytes;
        if(g->cfg.tls_size>size-offset)return 0;offset+=g->cfg.tls_size;count++;
    }
    if(offset!=size||h.count!=count)return 0;error_text(error,"");return 1;
}
int xrdbg_native_restore(XrDbgNativeGroup *g,const void *data,size_t size,char *error)
{
    size_t offset=sizeof(NativeHeader);
    if(!xrdbg_native_validate(g,data,size,error))return 0;
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active) {
        XrDbgNativeWorker *s=&g->workers[i];NativeEntry e;
        memcpy(&e,(char *)data+offset,sizeof e);offset+=sizeof e;
        memcpy((void *)(uintptr_t)e.stack_pointer,(char *)data+offset,(size_t)e.stack_bytes);offset+=(size_t)e.stack_bytes;
        memcpy(s->tls,(char *)data+offset,g->cfg.tls_size);offset+=g->cfg.tls_size;
        s->current=e.context;s->selected=e.context;s->loaded=1;
        s->suspend_count=e.suspend_count;s->resume_token=e.resume_token;
    }return 1;
}

int xrdbg_native_suspend(XrDbgNativeGroup *g,DWORD tid,ULONG *previous)
{
    XrDbgNativeWorker *s=NULL;int self=tid==GetCurrentThreadId();
    if(!g||!tid)return 0;
    AcquireSRWLockExclusive(&g->lock);
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active&&g->workers[i].tid==tid){s=&g->workers[i];break;}
    if(!s||s->suspend_count>=127){ReleaseSRWLockExclusive(&g->lock);return 0;}
    if(previous)*previous=s->suspend_count;
    if(self&&s->resume_token)s->resume_token=0;else s->suspend_count++;
    ReleaseSRWLockExclusive(&g->lock);
    if(self)xrdbg_native_checkpoint(s);
    return 1;
}
int xrdbg_native_resume(XrDbgNativeGroup *g,DWORD tid,ULONG *previous)
{
    XrDbgNativeWorker *s=NULL;
    if(!g||!tid)return 0;
    AcquireSRWLockExclusive(&g->lock);
    for(unsigned i=0;i<NATIVE_MAX_WORKERS;i++)if(g->workers[i].active&&g->workers[i].tid==tid){s=&g->workers[i];break;}
    if(!s){ReleaseSRWLockExclusive(&g->lock);return 0;}
    if(previous)*previous=s->suspend_count;
    if(s->suspend_count)s->suspend_count--;else s->resume_token=1;
    WakeAllConditionVariable(&g->changed);ReleaseSRWLockExclusive(&g->lock);return 1;
}
