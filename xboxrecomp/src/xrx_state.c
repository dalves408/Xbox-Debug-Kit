/* Debug-only save-state adapter (XRX_SAVE_STATES). F6 saves, F9 loads,
 * F7 runs the staged native probe. Snapshots are taken at a frame boundary:
 * the coordinator waits for a presented frame, then parks every guest and
 * device worker cooperatively before capture or restore. States are
 * same-process only (native continuations and host object identities). */
#ifdef XRDBG
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "xrdbg.h"
#include "xrdbg_state.h"
#include "xrx_state.h"
#include "xrx_state_runtime.h"
extern ptrdiff_t xbox_GetMemoryOffset(void);
volatile long g_xrx_pb_resync;
static XrDbgState *s_state;
static HANDLE s_request, s_worker, s_flip;
static volatile LONG s_operation, s_stop, s_shutdown;
static SRWLOCK s_dispatch_lock=SRWLOCK_INIT;
static char s_path[MAX_PATH];
static volatile LONG s_flips, s_watch_flip;
static volatile uint32_t s_fva,s_fw,s_fh,s_fpitch,s_fbpp;
static volatile LONG64 s_last_frame_hash;
static ULONGLONG s_paused_at;

static uint64_t frame_hash(uint32_t va,uint32_t w,uint32_t h,uint32_t pitch,uint32_t bpp)
{
    uint8_t *base; MEMORY_BASIC_INFORMATION mi; uint64_t acc=1469598103934665603ull; uint32_t y;
    if(!w||!h||!bpp||!pitch)return 0;
    base=(uint8_t*)(uintptr_t)va+xbox_GetMemoryOffset();
    if(!VirtualQuery(base,&mi,sizeof mi)||mi.State!=MEM_COMMIT||
       (uint8_t*)mi.BaseAddress+mi.RegionSize<base+(size_t)pitch*(h-1)+(size_t)w*bpp)return 0;
    for(y=0;y<h;y++)acc=(acc^xrx_state_hash(base+(size_t)y*pitch,(size_t)w*bpp))*1099511628211ull;
    return acc;
}
/* XRDBG_STATE_SHOT_DIR: write the presented frame as BMP at the saved moment
 * (save-NN-k0), at the restored moment (load-NN-k0), and SHOT_K presented
 * and SHOT_K2 presented frames after each save/load resume (save-NN-kK / load-NN-kK).
 * The late one shows whether the game really carries on after a load. */
#define SHOT_K 10
#define SHOT_K2 600
static volatile LONG s_shot_left, s_shot_op, s_shot_seq[3];
static void shot(const char *tag,long seq,int k,uint32_t va,uint32_t w,uint32_t h,uint32_t pitch,uint32_t bpp)
{
    const char *dir=getenv("XRDBG_STATE_SHOT_DIR"); char path[MAX_PATH]; FILE *fp; uint8_t *base,*row; uint32_t x,y;
    uint8_t hdr[54]={'B','M'}; uint32_t img=w*h*4,v;
    if(!dir||!*dir||!w||!h||(bpp!=4&&bpp!=2)||!frame_hash(va,w,h,pitch,bpp))return;
    snprintf(path,sizeof path,"%s\\%s-%02ld-k%d.bmp",dir,tag,seq,k);
    if(!(fp=fopen(path,"wb")))return;
    v=54+img;memcpy(hdr+2,&v,4);v=54;memcpy(hdr+10,&v,4);v=40;memcpy(hdr+14,&v,4);memcpy(hdr+18,&w,4);
    v=(uint32_t)(-(int32_t)h);memcpy(hdr+22,&v,4);hdr[26]=1;hdr[28]=32;memcpy(hdr+34,&img,4);
    fwrite(hdr,1,54,fp);
    base=(uint8_t*)(uintptr_t)va+xbox_GetMemoryOffset();
    row=(uint8_t*)malloc((size_t)w*4);
    for(y=0;row&&y<h;y++){
        uint8_t *s=base+(size_t)y*pitch;
        if(bpp==4){memcpy(row,s,(size_t)w*4);for(x=0;x<w;x++)row[x*4+3]=255;}
        else for(x=0;x<w;x++){uint16_t p=(uint16_t)(s[x*2]|s[x*2+1]<<8);row[x*4]=(uint8_t)((p&31)<<3);row[x*4+1]=(uint8_t)(((p>>5)&63)<<2);row[x*4+2]=(uint8_t)((p>>11)<<3);row[x*4+3]=255;}
        fwrite(row,1,(size_t)w*4,fp);
    }
    free(row);fclose(fp);
    fprintf(stderr,"[STATE] shot %s-%02ld-k%d flip=%ld frame=%016llX\n",tag,seq,k,s_flips,(unsigned long long)frame_hash(va,w,h,pitch,bpp));
}
void xrx_state_on_flip(uint32_t va,uint32_t w,uint32_t h,uint32_t pitch,uint32_t bpp)
{
    uint64_t fh;
    if(!w||!h)return;
    s_fva=va;s_fw=w;s_fh=h;s_fpitch=pitch;s_fbpp=bpp;
    fh=frame_hash(va,w,h,pitch,bpp);s_last_frame_hash=(LONG64)fh;
    InterlockedIncrement(&s_flips);
    if(InterlockedExchange(&s_watch_flip,0))
        fprintf(stderr,"[STATE] first presented frame after resume: flip=%ld frame=%016llX %ux%u va=%08X\n",
                s_flips,(unsigned long long)fh,w,h,va);
    if(s_shot_left){
        LONG left=InterlockedDecrement(&s_shot_left),op=s_shot_op;
        if(left==SHOT_K2-SHOT_K)shot(op==1?"save":"load",s_shot_seq[op],SHOT_K,va,w,h,pitch,bpp);
        else if(left==0)shot(op==1?"save":"load",s_shot_seq[op],SHOT_K2,va,w,h,pitch,bpp);
    }
    if(s_flip)SetEvent(s_flip);
}
static uint32_t capabilities(void *user, XrDbgStateOperation op, char *error)
{
    (void)user;(void)op;(void)error;
    return XRDBG_STATE_ALL;
}
static int quiesce(void *user,char *error)
{
    (void)user;
    int attempt;
    /* A device can be inside guest DPC work that waits on a parked guest; a
     * timed-out pause unparks everything, so retry at a later frame. */
    for(attempt=1;attempt<=5;attempt++){
        ResetEvent(s_flip);
        if(WaitForSingleObject(s_flip,500)!=WAIT_OBJECT_0){snprintf(error,XRDBG_STATE_ERROR_SIZE,"no presented frame within 500 ms");return 0;}
        if(xrx_state_pause(error)){
            if(attempt>1)fprintf(stderr,"[STATE] frame-boundary pause reached on attempt %d\n",attempt);
            s_paused_at=GetTickCount64();return 1;
        }
        fprintf(stderr,"[STATE] pause attempt %d failed: %s\n",attempt,error);
    }
    return 0;
}
static void resume(void *user)
{
    (void)user;
    InterlockedExchange(&s_watch_flip,1);
    {LONG op=s_operation;if(op==1||op==2){s_shot_op=op;InterlockedExchange(&s_shot_left,SHOT_K2);}}
    fprintf(stderr,"[STATE] resume after %llu ms paused\n",GetTickCount64()-s_paused_at);
    xrx_state_unpause();
}
/* The presented frame at the last save, to report on its load. Only saves record it:
 * each load takes a rollback capture first, which must not displace the slot's entry. */
typedef struct{uint64_t ram;uint32_t va,w,h,pitch,bpp;}FrameDesc;
static FrameDesc s_fd;
static int capture(void *user,void *data,size_t capacity,size_t *used,char *error)
{
    uint64_t ram=0,fh=frame_hash(s_fva,s_fw,s_fh,s_fpitch,s_fbpp);
    (void)user;
    if(!xrx_state_snap_capture(data,capacity,used,&ram,error))return 0;
    if(s_operation==1){FrameDesc d={ram,s_fva,s_fw,s_fh,s_fpitch,s_fbpp};s_fd=d;}
    if(s_operation==1)shot("save",++s_shot_seq[1],0,s_fva,s_fw,s_fh,s_fpitch,s_fbpp);
    fprintf(stderr,"[STATE] capture: flip=%ld ram=%016llX frame=%016llX bytes=%zu\n",
            s_flips,(unsigned long long)ram,(unsigned long long)fh,*used);
    return 1;
}
static int validate(void *user,const void *data,size_t size,char *error){(void)user;return xrx_state_snap_validate(data,size,error);}
static int restore(void *user,const void *data,size_t size,char *error)
{
    uint64_t ram=0,fh;
    (void)user;
    if(!xrx_state_snap_restore(data,size,&ram,error))return 0;
    InterlockedExchange((volatile LONG*)&g_xrx_pb_resync,1);
    {FrameDesc d={0,s_fva,s_fw,s_fh,s_fpitch,s_fbpp};if(s_fd.ram==ram&&s_fd.w)d=s_fd;
     fh=frame_hash(d.va,d.w,d.h,d.pitch,d.bpp);shot("load",++s_shot_seq[2],0,d.va,d.w,d.h,d.pitch,d.bpp);} /* the snapshot's presented buffer, not the current one */
    fprintf(stderr,"[STATE] restore: flip=%ld ram=%016llX frame=%016llX bytes=%zu\n",
            s_flips,(unsigned long long)ram,(unsigned long long)fh,size);
    return 1;
}
static void fatal(void *user,const char *error)
{
    (void)user;
    fprintf(stderr,"[FATAL] [STATE] restore and rollback failed: %s\n",error);fflush(stderr);
    TerminateProcess(GetCurrentProcess(),0xDEAD);
}
/* XRDBG_STATE_SCHEDULE="ms:op,..." with op save|load|probe, ms from adapter
 * start. XRDBG_STATE_NATIVE_PROBE_MS="ms,ms" remains a probe-only schedule. */
typedef struct { ULONGLONG at; LONG op; } Planned;
static Planned s_plan[64]; static int s_nplan;
static void plan_parse(void)
{
    const char *e=getenv("XRDBG_STATE_SCHEDULE"),*p=getenv("XRDBG_STATE_NATIVE_PROBE_MS");
    char *next;
    while(e&&*e&&s_nplan<64){
        unsigned long ms=strtoul(e,&next,10);LONG op=0;
        if(*next==':'){next++;
            if(!strncmp(next,"save",4))op=XRDBG_STATE_SAVE,next+=4;
            else if(!strncmp(next,"load",4))op=XRDBG_STATE_LOAD,next+=4;
            else if(!strncmp(next,"probe",5))op=3,next+=5;}
        if(!op){fprintf(stderr,"[STATE] bad XRDBG_STATE_SCHEDULE near '%s'\n",e);break;}
        s_plan[s_nplan].at=ms;s_plan[s_nplan++].op=op;
        e=*next==','?next+1:next;
    }
    while(p&&*p&&s_nplan<64){unsigned long ms=strtoul(p,&next,10);if(next==p)break;
        s_plan[s_nplan].at=ms;s_plan[s_nplan++].op=3;p=*next==','?next+1:next;}
}
static DWORD WINAPI coordinator(void *unused)
{
    ULONGLONG started=GetTickCount64();int k=0;
    (void)unused;
    plan_parse();
    for (;;) {
        LONG operation;
        char error[XRDBG_STATE_ERROR_SIZE];
        XrDbgStateResult result;
        ULONGLONG now=GetTickCount64();
        DWORD delay=k<s_nplan?(now>=started+s_plan[k].at?0:(DWORD)(started+s_plan[k].at-now)):INFINITE;
        DWORD wait=WaitForSingleObject(s_request,delay);
        if(wait==WAIT_TIMEOUT){
            if(InterlockedCompareExchange(&s_operation,s_plan[k].op,0))fprintf(stderr,"[STATE] scheduled op skipped: busy\n");
            else fprintf(stderr,"[STATE] scheduled %s at %llu ms\n",s_plan[k].op==1?"save":s_plan[k].op==2?"load":"probe",s_plan[k].at);
            k++;
        } else if(wait!=WAIT_OBJECT_0)return 1;
        if (InterlockedCompareExchange(&s_stop,0,0)) return 0;
        operation=InterlockedCompareExchange(&s_operation,0,0);
        if (!operation) continue;
        if(operation==3) {
            int ok=xrx_state_native_probe(error);
            fprintf(stderr,"[STATE] native probe: %s %s\n",ok?"ok":"failed",error);
            InterlockedExchange(&s_operation,0);continue;
        }
        {
            uint64_t before=operation==XRDBG_STATE_LOAD?xrx_state_ram_hash():0;
            ULONGLONG t0=GetTickCount64();
            result=xrdbg_state_run(s_state,(XrDbgStateOperation)operation,s_path,error);
            fprintf(stderr,"[STATE] %s: %s%s%s (%llu ms",operation==XRDBG_STATE_SAVE?"save":"load",
                    xrdbg_state_result_name(result),*error?": ":"",error,GetTickCount64()-t0);
            if(operation==XRDBG_STATE_LOAD)fprintf(stderr,", ram before load (unpaused)=%016llX",(unsigned long long)before);
            fprintf(stderr,")\n");
            /* The person at the game has no other sign that F6/F9 did anything. */
            if(result==XRDBG_STATE_OK)xrdbg_banner(operation==XRDBG_STATE_SAVE?"STATE SAVED":"STATE LOADED",operation==XRDBG_STATE_SAVE?"F9 returns here (until the game closes)":"Back at the saved moment",1500,RGB(60,200,80));
            else xrdbg_banner(operation==XRDBG_STATE_SAVE?"SAVE FAILED":"LOAD FAILED",*error?error:xrdbg_state_result_name(result),3000,RGB(230,70,60));
        }
        InterlockedExchange(&s_operation,0);
    }
}
void (*g_xrx_after_load)(void);
void xrx_state_init(const XrxStateConfig *config)
{
    XrDbgStateConfig cfg={0};
    char runtime_error[256];
    size_t need;
    if(!xrx_state_runtime_init(runtime_error))
        fprintf(stderr,"[STATE] native backend unavailable: %s\n",runtime_error);
    need=xrx_state_snap_max();
    if(need<160u*1024u*1024u)need=160u*1024u*1024u;
    if(need>512u*1024u*1024u)need=512u*1024u*1024u;
    g_xrx_after_load=config->after_load;
    cfg.game_id=config->game_id;
    cfg.build_id=config->build_id; /* same-process states; any rebuild invalidates them */
    cfg.schema=4; cfg.max_payload=need; cfg.capabilities=capabilities;
    cfg.quiesce=quiesce; cfg.resume=resume; cfg.capture=capture; cfg.validate=validate; cfg.restore=restore; cfg.fatal=fatal;
    snprintf(s_path,sizeof s_path,"%s\\quickstate.xrstate",xrdbg_dir());
    s_flip=CreateEventA(NULL,FALSE,FALSE,NULL);
    s_state=s_flip?xrdbg_state_create(&cfg):NULL;
    if (s_state) s_request=CreateEventA(NULL,FALSE,FALSE,NULL);
    if (s_request) s_worker=CreateThread(NULL,0,coordinator,NULL,0,NULL);
    if (!s_worker) {
        if (s_request) CloseHandle(s_request);
        s_request=NULL; xrdbg_state_destroy(s_state); s_state=NULL;
        fprintf(stderr,"[STATE] coordinator unavailable\n"); return;
    }
    fprintf(stderr,"[STATE] experimental frame-boundary save states: F6 save / F9 load (same process only), F7 native probe; "
                   "slot %s, payload limit %zu MiB\n",s_path,need>>20);
}
void xrx_state_keys(int focused)
{
    static int save_was_down, load_was_down, probe_was_down;
    int save=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
    int load=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    int probe=(GetAsyncKeyState(VK_F7)&0x8000)!=0;
    LONG op=0;
    if (focused && save && !save_was_down) op=XRDBG_STATE_SAVE;
    else if (focused && load && !load_was_down) op=XRDBG_STATE_LOAD;
    else if (focused && probe && !probe_was_down) op=3;
    save_was_down=save; load_was_down=load; probe_was_down=probe;
    AcquireSRWLockShared(&s_dispatch_lock);
    if (op && s_request && !InterlockedCompareExchange(&s_stop,0,0)) {
        if (!InterlockedCompareExchange(&s_operation,op,0)) SetEvent(s_request);
        else fprintf(stderr,"[STATE] request ignored: operation already pending\n");
    }
    ReleaseSRWLockShared(&s_dispatch_lock);
}
void xrx_state_shutdown(void)
{
    if (InterlockedCompareExchange(&s_shutdown,1,0)) return;
    AcquireSRWLockExclusive(&s_dispatch_lock);
    if (!s_worker) { ReleaseSRWLockExclusive(&s_dispatch_lock); return; }
    InterlockedExchange(&s_stop,1); SetEvent(s_request);
    ReleaseSRWLockExclusive(&s_dispatch_lock);
    if (WaitForSingleObject(s_worker,1000)==WAIT_OBJECT_0) {
        AcquireSRWLockExclusive(&s_dispatch_lock);
        CloseHandle(s_worker); s_worker=NULL;
        CloseHandle(s_request); s_request=NULL;
        xrdbg_state_destroy(s_state); s_state=NULL;
        xrx_state_runtime_shutdown();
        ReleaseSRWLockExclusive(&s_dispatch_lock);
    }
}
#endif
