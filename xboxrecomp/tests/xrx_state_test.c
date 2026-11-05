/* Exercise the real adapter's save/load path against a stub runtime: the
 * frame-boundary quiesce, the engine's save, a mutating load with rollback
 * capture, and the refusal without presented frames. No game files needed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/xrx_state.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint8_t guest[4096]; static uint32_t pixels[16]; static int paused, pauses;
int xrx_state_runtime_init(char *error){*error=0;return 1;}
void xrx_state_runtime_shutdown(void){}
int xrx_state_native_probe(char *error){*error=0;return 1;}
int xrx_state_pause(char *error){*error=0;paused=1;pauses++;return 1;}
void xrx_state_unpause(void){paused=0;}
size_t xrx_state_snap_max(void){return sizeof guest;}
uint64_t xrx_state_hash(const void *d,size_t n){const uint8_t *b=d;uint64_t h=1469598103934665603ull;while(n--){h^=*b++;h*=1099511628211ull;}return h;}
uint64_t xrx_state_ram_hash(void){return xrx_state_hash(guest,sizeof guest);}
int xrx_state_snap_capture(void *buf,size_t cap,size_t *used,uint64_t *ram,char *error)
{ if(!paused||cap<sizeof guest){strcpy(error,"bad capture");return 0;} memcpy(buf,guest,sizeof guest);*used=sizeof guest;*ram=xrx_state_ram_hash();return 1; }
int xrx_state_snap_validate(const void *buf,size_t n,char *error){(void)buf;if(!paused||n!=sizeof guest){strcpy(error,"bad validate");return 0;}return 1;}
int xrx_state_snap_restore(const void *buf,size_t n,uint64_t *ram,char *error)
{ if(!paused||n!=sizeof guest){strcpy(error,"bad restore");return 0;} memcpy(guest,buf,n);*ram=xrx_state_ram_hash();return 1; }
ptrdiff_t xbox_GetMemoryOffset(void){return (ptrdiff_t)((uintptr_t)pixels-0x1000u);}
static char directory[MAX_PATH];
const char *xrdbg_dir(void) { return directory; }
void xrdbg_banner(const char *title, const char *line, unsigned ms, unsigned long accent) { (void)title; (void)line; (void)ms; (void)accent; }
static volatile LONG flipping;
static DWORD WINAPI flipper(void *u){(void)u;while(flipping){xrx_state_on_flip(0x1000,4,4,16,4);Sleep(2);}return 0;}
int main(void)
{
    char temp[MAX_PATH], error[XRDBG_STATE_ERROR_SIZE];
    DWORD start; HANDLE t;
    CHECK(GetTempPathA(sizeof temp,temp));
    CHECK(GetTempFileNameA(temp,"mvs",0,directory));
    CHECK(DeleteFileA(directory)); CHECK(CreateDirectoryA(directory,NULL));
    {XrxStateConfig c={"test-game","test-build",NULL};xrx_state_init(&c);} CHECK(s_state && s_worker);
    /* No presented frames: refuse before pausing anything, write nothing. */
    CHECK(xrdbg_state_run(s_state,XRDBG_STATE_SAVE,s_path,error)!=XRDBG_STATE_OK);
    CHECK(strstr(error,"presented frame")); CHECK(!pauses);
    CHECK(GetFileAttributesA(s_path)==INVALID_FILE_ATTRIBUTES);
    flipping=1; CHECK(t=CreateThread(NULL,0,flipper,NULL,0,NULL));
    memset(guest,0x11,sizeof guest);
    CHECK(xrdbg_state_run(s_state,XRDBG_STATE_SAVE,s_path,error)==XRDBG_STATE_OK);
    CHECK(GetFileAttributesA(s_path)!=INVALID_FILE_ATTRIBUTES); CHECK(!paused);
    memset(guest,0x22,sizeof guest);
    CHECK(xrdbg_state_run(s_state,XRDBG_STATE_LOAD,s_path,error)==XRDBG_STATE_OK);
    CHECK(guest[0]==0x11 && guest[sizeof guest-1]==0x11); CHECK(!paused);
    /* The key-handler event path dispatches a load through the coordinator. */
    memset(guest,0x33,sizeof guest);
    CHECK(!InterlockedCompareExchange(&s_operation,XRDBG_STATE_LOAD,0));
    CHECK(SetEvent(s_request)); start=GetTickCount();
    while (InterlockedCompareExchange(&s_operation,0,0) && GetTickCount()-start<5000) Sleep(1);
    CHECK(!InterlockedCompareExchange(&s_operation,0,0)); CHECK(guest[7]==0x11);
    flipping=0; WaitForSingleObject(t,INFINITE); CloseHandle(t);
    xrx_state_shutdown(); CHECK(!s_state && !s_worker && !s_request);
    CHECK(DeleteFileA(s_path)); CHECK(RemoveDirectoryA(directory));
    puts("PASS: adapter saves at a presented frame, loads, refuses without frames, dispatches and shuts down");
    return 0;
}
