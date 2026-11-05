/* guest execution layer. Kernel/device state is deliberately separate.
 * Instrumented kernel copies are generated at configure time; the shared
 * toolkit checkout and release configuration remain untouched. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "xrdbg_native.h"
#include "xrx_state_runtime.h"
static XrDbgNativeGroup *s_group,*s_devices;
extern int xbox_IrqlCurrent(void);
extern int xbox_GuestCpuHeld(void);
static __declspec(thread) XrDbgNativeWorker *t_worker;
static __declspec(thread) int t_device;
typedef struct GuestTls {
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esp;
    uint32_t ebx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t fs_base;
    uint32_t seh_ebp;
    uint32_t eflags;
    uint32_t mxcsr;
    int fp_top;
    int df;
    int fp_cmp;
    uint16_t fp_control_word;
    uint16_t fp_cc;
    double fp_stack[8];
    RecompXmm xmm[8];
} GuestTls;
static void capture_tls(void *data)
{
    GuestTls *s=data;
    memset(s,0,sizeof *s);
    s->eax=g_eax;
    s->ecx=g_ecx;
    s->edx=g_edx;
    s->esp=g_esp;
    s->ebx=g_ebx;
    s->esi=g_esi;
    s->edi=g_edi;
    s->ebp=g_ebp;
    s->fs_base=g_fs_base;
    s->seh_ebp=g_seh_ebp;
    s->eflags=g_eflags;
    s->mxcsr=g_mxcsr;
    s->fp_top=g_fp_top;
    s->df=g_df;
    s->fp_cmp=g_fp_cmp;
    s->fp_control_word=g_fp_control_word;
    s->fp_cc=g_fp_cc;
    memcpy(s->fp_stack,g_fp_stack,sizeof s->fp_stack);
    s->xmm[0]=g_xmm0;s->xmm[1]=g_xmm1;s->xmm[2]=g_xmm2;s->xmm[3]=g_xmm3;
    s->xmm[4]=g_xmm4;s->xmm[5]=g_xmm5;s->xmm[6]=g_xmm6;s->xmm[7]=g_xmm7;
}
static void restore_tls(const void *data)
{
    const GuestTls *s=data;
    g_eax=s->eax;
    g_ecx=s->ecx;
    g_edx=s->edx;
    g_esp=s->esp;
    g_ebx=s->ebx;
    g_esi=s->esi;
    g_edi=s->edi;
    g_ebp=s->ebp;
    g_fs_base=s->fs_base;
    g_seh_ebp=s->seh_ebp;
    g_eflags=s->eflags;
    g_mxcsr=s->mxcsr;
    g_fp_top=s->fp_top;
    g_df=s->df;
    g_fp_cmp=s->fp_cmp;
    g_fp_control_word=s->fp_control_word;
    g_fp_cc=s->fp_cc;
    memcpy(g_fp_stack,s->fp_stack,sizeof s->fp_stack);
    g_xmm0=s->xmm[0];g_xmm1=s->xmm[1];g_xmm2=s->xmm[2];g_xmm3=s->xmm[3];
    g_xmm4=s->xmm[4];g_xmm5=s->xmm[5];g_xmm6=s->xmm[6];g_xmm7=s->xmm[7];
}
int xrx_state_runtime_init(char *error)
{
    XrDbgNativeConfig cfg={sizeof(GuestTls),capture_tls,restore_tls};
    s_group=xrdbg_native_create(&cfg,error);
    if(!s_group)return 0;
    s_devices=xrdbg_native_create(&cfg,error);
    if(!s_devices){xrdbg_native_destroy(s_group);s_group=NULL;return 0;}
    return 1;
}
void xrx_state_runtime_shutdown(void)
{
    /* The main guest is still active on a normal game exit. Retain its group
     * until process teardown; never free objects a guest thread can still use. */
    if(s_devices&&!xrdbg_native_count(s_devices)){xrdbg_native_destroy(s_devices);s_devices=NULL;}
    if(s_group && !xrdbg_native_count(s_group)){xrdbg_native_destroy(s_group);s_group=NULL;}
}
void xrx_state_guest_join(void)
{
    if(s_group&&!t_worker) {
        t_worker=xrdbg_native_register(s_group,"xrx-guest");
        fprintf(stderr,"[STATE] native guest registration tid=%lu %s\n",GetCurrentThreadId(),t_worker?"ok":"failed");
    }
}
void xrx_state_guest_part(void)
{
    if(t_worker){xrdbg_native_unregister(t_worker);t_worker=NULL;}
}
void xrx_state_guest_location(unsigned location) {xrdbg_native_location(t_worker,location);}
void xrx_state_guest_checkpoint(void)
{
    if(t_worker&&!t_device&&!xbox_IrqlCurrent()&&!xbox_GuestCpuHeld())xrdbg_native_checkpoint(t_worker);
}
unsigned xrx_state_runtime_count(void) {return xrdbg_native_count(s_group);}
int xrx_state_native_probe(char *error)
{
    void *guest=NULL,*devices=NULL;size_t guest_bytes=0,device_bytes=0;
    int ok,echo=getenv("XRDBG_STATE_NATIVE_PROBE_RESTORE")!=NULL;
    if(!xrdbg_native_begin(s_group,2000,error))return 0;
    if(!xrdbg_native_begin(s_devices,2000,error)){xrdbg_native_end(s_group);return 0;}
    ok=xrdbg_native_capture(s_group,&guest,&guest_bytes,error);
    if(ok)ok=xrdbg_native_capture(s_devices,&devices,&device_bytes,error);
    if(ok)ok=xrdbg_native_validate(s_group,guest,guest_bytes,error)&&xrdbg_native_validate(s_devices,devices,device_bytes,error);
    if(ok&&echo)ok=xrdbg_native_restore(s_group,guest,guest_bytes,error)&&xrdbg_native_restore(s_devices,devices,device_bytes,error);
    fprintf(stderr,"[STATE] native pause/capture probe: guests=%u devices=%u bytes=%zu validated=%d echo_restore=%d\n",
            xrdbg_native_count(s_group),xrdbg_native_count(s_devices),guest_bytes+device_bytes,ok,echo);
    xrdbg_native_end(s_devices);xrdbg_native_end(s_group);free(guest);free(devices);return ok;
}
/* Poll only the guest wait primitives whose callers hold no object locks.
 * Keep Windows WaitAll/APC/abandoned/failure results intact and count finite
 * timeout duration outside cooperative pauses. Zero waits stay a direct call. */
DWORD xrx_state_wait_multiple(DWORD count,const HANDLE *objects,BOOL all,DWORD ms,BOOL alertable)
{
    ULONGLONG deadline=GetTickCount64()+ms;
    if(!t_worker||t_device||!ms)return WaitForMultipleObjectsEx(count,objects,all,ms,alertable);
    for(;;) {
        ULONGLONG before=GetTickCount64(),now;
        DWORD slice,result;
        xrx_state_guest_checkpoint();now=GetTickCount64();deadline+=now-before;
        if(ms!=INFINITE&&now>=deadline)return WaitForMultipleObjectsEx(count,objects,all,0,alertable);
        slice=ms==INFINITE?10:(DWORD)(deadline-now);if(slice>10)slice=10;
        result=WaitForMultipleObjectsEx(count,objects,all,slice,alertable);
        if(result!=WAIT_TIMEOUT)return result;
    }
}
DWORD xrx_state_wait_single(HANDLE object,DWORD ms,BOOL alertable)
{
    return xrx_state_wait_multiple(1,&object,FALSE,ms,alertable);
}

int xrx_state_thread_suspend(HANDLE handle,ULONG *previous)
{
    xrx_state_guest_checkpoint();
    return xrdbg_native_suspend(s_group,GetThreadId(handle),previous);
}
int xrx_state_thread_resume(HANDLE handle,ULONG *previous)
{
    xrx_state_guest_checkpoint();
    return xrdbg_native_resume(s_group,GetThreadId(handle),previous);
}

void xrx_state_device_join(const char *name)
{
    if(s_devices&&!t_worker){t_worker=xrdbg_native_register(s_devices,name);t_device=1;
        fprintf(stderr,"[STATE] native device registration tid=%lu role=%s %s\n",GetCurrentThreadId(),name,t_worker?"ok":"failed");}
}
void xrx_state_device_checkpoint(void){if(t_worker&&t_device)xrdbg_native_checkpoint(t_worker);}
void xrx_state_device_part(void){if(t_worker&&t_device){xrdbg_native_unregister(t_worker);t_worker=NULL;t_device=0;}}
DWORD xrx_state_device_wait(HANDLE event,DWORD ms)
{
    ULONGLONG deadline=GetTickCount64()+ms;
    for(;;){
        DWORD slice,result;ULONGLONG now;
        xrx_state_device_checkpoint();now=GetTickCount64();
        if(ms!=INFINITE&&now>=deadline)return WaitForSingleObject(event,0);
        slice=ms==INFINITE?10:(DWORD)(deadline-now);if(slice>10)slice=10;
        result=WaitForSingleObject(event,slice);if(result!=WAIT_TIMEOUT)return result;
    }
}

void xrx_state_heartbeat(uint32_t flips)
{
    extern void xrx_cs_log_held(void); extern void xrx_apu_log_state(void);
    static uint32_t last_flips;
    if(getenv("XRDBG_STATE_LOCKLOG")||(flips&&flips==last_flips)){xrx_cs_log_held();xrx_apu_log_state();}
    last_flips=flips;
}

/* ---- Frame-boundary snapshot. ----
 * Payload: header, then tagged sections. Native continuations (guest and
 * device groups), logical kernel objects by stable slot, allocator metadata,
 * then whole guest memory regions (RAM, contiguous window, NV2A aperture).
 * Same-process only: native stacks and host object identities are checked. */
typedef struct { const char *name; void *host; size_t size; } XrxMemRegion;
extern int xrx_mstate_regions(XrxMemRegion *out,int max);
extern size_t xrx_mstate_meta_size(void);
extern void xrx_mstate_meta_capture(void *buf);
extern void xrx_mstate_meta_restore(const void *buf);
extern size_t xrx_kstate_max(void);
extern int xrx_kstate_capture(void *buf,size_t cap,size_t *used,char *err,size_t errn);
extern int xrx_kstate_validate(const void *buf,size_t n,char *err,size_t errn);
extern int xrx_kstate_restore(const void *buf,size_t n,int *mutant_mismatch,char *err,size_t errn);
extern void (*g_xrx_after_load)(void);
extern size_t xrx_astate_size(void);
extern void xrx_astate_capture(void *buf);
extern void xrx_astate_restore(const void *buf);
extern size_t xrx_csstate_max(void);
extern size_t xrx_csstate_capture(void *buf);
extern int xrx_csstate_validate(const void *buf,size_t n,char *err,size_t errn);
extern void xrx_csstate_restore(const void *buf);
extern size_t xrx_ustate_size(void);
extern void xrx_ustate_capture(void *buf);
extern void xrx_ustate_restore(const void *buf);
#define SNAP_MAGIC 0x50325653u
#define SNAP_ERR 256
typedef struct { uint32_t magic,version,nsect,pad; uint64_t ram_hash; } SnapHeader;
typedef struct { uint32_t tag,index; uint64_t size; } SnapSection;
enum { SEC_GUESTS=1, SEC_DEVICES, SEC_KERNEL, SEC_META, SEC_APU, SEC_USB, SEC_LOCKS, SEC_REGION };
#define SNAP_FIXED 7 /* sections before the memory regions */
#define SNAP_VERSION 3
static int s_paused;
uint64_t xrx_state_hash(const void *data,size_t n)
{
    const uint64_t *w=data; const uint8_t *b; uint64_t h=1469598103934665603ull; size_t i;
    for(i=0;i<n/8;i++){h^=w[i];h*=1099511628211ull;}
    for(b=(const uint8_t*)(w+n/8),i=0;i<n%8;i++){h^=b[i];h*=1099511628211ull;}
    return h;
}
uint64_t xrx_state_ram_hash(void)
{
    XrxMemRegion r[4];
    return xrx_mstate_regions(r,4)>0?xrx_state_hash(r[0].host,r[0].size):0;
}
size_t xrx_state_snap_max(void)
{
    XrxMemRegion r[4];int i,n=xrx_mstate_regions(r,4);size_t total=sizeof(SnapHeader)+8*sizeof(SnapSection)+xrx_kstate_max()+xrx_mstate_meta_size()+xrx_astate_size()+xrx_ustate_size()+xrx_csstate_max()+(8u<<20);
    for(i=0;i<n;i++)total+=r[i].size+sizeof(SnapSection);
    return total;
}
int xrx_state_pause(char *error)
{
    if(!s_group||!s_devices){snprintf(error,SNAP_ERR,"native groups unavailable");return 0;}
    if(!xrdbg_native_begin(s_group,1000,error))return 0;
    if(!xrdbg_native_begin(s_devices,1000,error)){xrdbg_native_end(s_group);return 0;}
    s_paused=1;return 1;
}
void xrx_state_unpause(void)
{
    if(!s_paused)return;
    xrdbg_native_end(s_devices);xrdbg_native_end(s_group);s_paused=0;
}
static uint8_t *put_section(uint8_t *p,uint8_t *end,uint32_t tag,uint32_t index,const void *data,uint64_t size)
{
    SnapSection s={tag,index,size};
    if(!p||(uint64_t)(end-p)<sizeof s+size)return NULL;
    memcpy(p,&s,sizeof s);p+=sizeof s;if(size)memcpy(p,data,(size_t)size);return p+size;
}
int xrx_state_snap_capture(void *buf,size_t cap,size_t *used,uint64_t *ram_hash,char *error)
{
    uint8_t *p=buf,*end=p+cap;SnapHeader *h=buf;XrxMemRegion r[4];
    void *guest=NULL,*devices=NULL;size_t gn=0,dn=0,kn=0;int i,n;uint8_t *k;
    if(!s_paused){snprintf(error,SNAP_ERR,"capture outside pause");return 0;}
    if(cap<sizeof *h){snprintf(error,SNAP_ERR,"payload too small");return 0;}
    memset(h,0,sizeof *h);h->magic=SNAP_MAGIC;h->version=SNAP_VERSION;p+=sizeof *h;
    if(!xrdbg_native_capture(s_group,&guest,&gn,error)||!xrdbg_native_capture(s_devices,&devices,&dn,error)){free(guest);free(devices);return 0;}
    p=put_section(p,end,SEC_GUESTS,0,guest,gn);p=put_section(p,end,SEC_DEVICES,0,devices,dn);free(guest);free(devices);
    k=malloc(xrx_kstate_max());
    if(!k||!xrx_kstate_capture(k,xrx_kstate_max(),&kn,error,SNAP_ERR)){free(k);if(!*error)snprintf(error,SNAP_ERR,"kernel capture failed");return 0;}
    p=put_section(p,end,SEC_KERNEL,0,k,kn);free(k);
    k=malloc(xrx_mstate_meta_size());if(!k){snprintf(error,SNAP_ERR,"out of memory");return 0;}
    xrx_mstate_meta_capture(k);p=put_section(p,end,SEC_META,0,k,xrx_mstate_meta_size());free(k);
    k=malloc(xrx_astate_size());if(!k){snprintf(error,SNAP_ERR,"out of memory");return 0;}
    xrx_astate_capture(k);p=put_section(p,end,SEC_APU,0,k,xrx_astate_size());free(k);
    k=malloc(xrx_ustate_size());if(!k){snprintf(error,SNAP_ERR,"out of memory");return 0;}
    xrx_ustate_capture(k);p=put_section(p,end,SEC_USB,0,k,xrx_ustate_size());free(k);
    k=malloc(xrx_csstate_max());if(!k){snprintf(error,SNAP_ERR,"out of memory");return 0;}
    {size_t cn=xrx_csstate_capture(k);p=put_section(p,end,SEC_LOCKS,0,k,cn);}free(k);
    n=xrx_mstate_regions(r,4);
    for(i=0;i<n;i++)p=put_section(p,end,SEC_REGION,(uint32_t)i,r[i].host,r[i].size);
    if(!p){snprintf(error,SNAP_ERR,"payload limit exceeded");return 0;}
    h->nsect=SNAP_FIXED+(uint32_t)n;h->ram_hash=n?xrx_state_hash(r[0].host,r[0].size):0;
    if(ram_hash)*ram_hash=h->ram_hash;*used=(size_t)(p-(uint8_t*)buf);return 1;
}
static const SnapSection *next_section(const uint8_t **p,const uint8_t *end)
{
    const SnapSection *s=(const SnapSection*)*p;
    if((size_t)(end-*p)<sizeof *s||(uint64_t)(end-*p-sizeof *s)<s->size)return NULL;
    *p+=sizeof *s+s->size;return s;
}
static int walk(const void *buf,size_t n,int apply,uint64_t *ram_hash,char *error)
{
    const uint8_t *p=buf,*end=p+n;const SnapHeader *h=buf;XrxMemRegion r[4];int nr=xrx_mstate_regions(r,4),mm=0;uint32_t i;
    const SnapSection *sec[16]={0};
    if(n<sizeof *h||h->magic!=SNAP_MAGIC||h->version!=SNAP_VERSION||h->nsect!=SNAP_FIXED+(uint32_t)nr){snprintf(error,SNAP_ERR,"snapshot header mismatch");return 0;}
    p+=sizeof *h;
    for(i=0;i<h->nsect;i++){const SnapSection *s=next_section(&p,end);if(!s){snprintf(error,SNAP_ERR,"truncated section %u",i);return 0;}sec[i]=s;}
    if(p!=end||sec[0]->tag!=SEC_GUESTS||sec[1]->tag!=SEC_DEVICES||sec[2]->tag!=SEC_KERNEL||sec[3]->tag!=SEC_META||
       sec[3]->size!=xrx_mstate_meta_size()||sec[4]->tag!=SEC_APU||sec[4]->size!=xrx_astate_size()||
       sec[5]->tag!=SEC_USB||sec[5]->size!=xrx_ustate_size()||sec[6]->tag!=SEC_LOCKS){snprintf(error,SNAP_ERR,"snapshot layout mismatch");return 0;}
    for(i=0;i<(uint32_t)nr;i++)if(sec[SNAP_FIXED+i]->tag!=SEC_REGION||sec[SNAP_FIXED+i]->index!=i||sec[SNAP_FIXED+i]->size!=r[i].size){snprintf(error,SNAP_ERR,"region %s mismatch",r[i].name);return 0;}
    if(!xrdbg_native_validate(s_group,sec[0]+1,(size_t)sec[0]->size,error)||!xrdbg_native_validate(s_devices,sec[1]+1,(size_t)sec[1]->size,error))return 0;
    if(!xrx_kstate_validate(sec[2]+1,(size_t)sec[2]->size,error,SNAP_ERR))return 0;
    if(!xrx_csstate_validate(sec[6]+1,(size_t)sec[6]->size,error,SNAP_ERR))return 0;
    if(!apply)return 1;
    for(i=0;i<(uint32_t)nr;i++)memcpy(r[i].host,sec[SNAP_FIXED+i]+1,r[i].size);
    xrx_mstate_meta_restore(sec[3]+1);
    if(!xrx_kstate_restore(sec[2]+1,(size_t)sec[2]->size,&mm,error,SNAP_ERR))return 0;
    xrx_astate_restore(sec[4]+1);xrx_ustate_restore(sec[5]+1);xrx_csstate_restore(sec[6]+1);
    if(!xrdbg_native_restore(s_group,sec[0]+1,(size_t)sec[0]->size,error)||!xrdbg_native_restore(s_devices,sec[1]+1,(size_t)sec[1]->size,error))return 0;
    if(g_xrx_after_load)g_xrx_after_load();
    if(mm)fprintf(stderr,"[STATE] warning: %d mutant ownership counts differ from the snapshot\n",mm);
    if(ram_hash)*ram_hash=nr?xrx_state_hash(r[0].host,r[0].size):0;
    return 1;
}
int xrx_state_snap_validate(const void *buf,size_t n,char *error)
{
    if(!s_paused){snprintf(error,SNAP_ERR,"validate outside pause");return 0;}
    return walk(buf,n,0,NULL,error);
}
int xrx_state_snap_restore(const void *buf,size_t n,uint64_t *ram_hash,char *error)
{
    if(!s_paused){snprintf(error,SNAP_ERR,"restore outside pause");return 0;}
    return walk(buf,n,1,ram_hash,error);
}
