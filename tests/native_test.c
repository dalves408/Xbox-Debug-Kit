#include "xrdbg_native.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s (win32 %lu)\n",__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static XrDbgNativeGroup *group;
static volatile LONG stop, stall, depth=3, verified;
static volatile LONG64 observed[3];
static volatile LONG ids[3],seen_depth[3],self_request,self_done;
static __declspec(thread) XrDbgNativeWorker *worker;
static __declspec(thread) uint64_t tls_counter;
static __declspec(thread) int loaded;
static void capture_tls(void *data){memcpy(data,&tls_counter,sizeof tls_counter);}
static void restore_tls(const void *data){memcpy(&tls_counter,data,sizeof tls_counter);loaded=1;}
static __declspec(noinline) uint64_t step(unsigned n,uint64_t counter)
{
    volatile uint64_t canary[16];
    volatile double fp=(double)counter/13.0;
    uint64_t result;
    for(unsigned i=0;i<16;i++)canary[i]=counter+i+n*1000;
    if(n)result=step(n-1,counter);
    else {
        xrdbg_native_checkpoint(worker);
        CHECK(tls_counter==counter);
        if(loaded){loaded=0;InterlockedIncrement(&verified);}
        result=counter+1;
    }
    for(unsigned i=0;i<16;i++)CHECK(canary[i]==counter+i+n*1000);
    CHECK(fp==(double)counter/13.0);
    return result;
}
static DWORD WINAPI run_worker(void *arg)
{
    unsigned index=(unsigned)(uintptr_t)arg;
    uint64_t counter=index*10000;
    worker=xrdbg_native_register(group,"test-guest");CHECK(worker);
    InterlockedExchange(&ids[index],(LONG)GetCurrentThreadId());
    while(!InterlockedCompareExchange(&stop,0,0)) {
        tls_counter=counter;
        if(index==2&&InterlockedCompareExchange(&stall,0,0))Sleep(150);
        if(index==1&&InterlockedExchange(&self_request,0)) {
            ULONG before;CHECK(xrdbg_native_suspend(group,GetCurrentThreadId(),&before));CHECK(!before);
            InterlockedIncrement(&self_done);
        }
        InterlockedExchange(&seen_depth[index],depth);
        counter=step((unsigned)depth,counter);
        InterlockedExchange64(&observed[index],(LONG64)counter);
        Sleep(1);
    }
    xrdbg_native_unregister(worker);return 0;
}
static void wait_count(unsigned count)
{
    ULONGLONG end=GetTickCount64()+5000;
    while(xrdbg_native_count(group)!=count&&GetTickCount64()<end)Sleep(1);
    CHECK(xrdbg_native_count(group)==count);
}
static LONG WINAPI fault(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr,"FAULT code=%08lX rip=%p rsp=%p addr=%p worker=%p verified=%ld\n",ep->ExceptionRecord->ExceptionCode,
        (void *)ep->ContextRecord->Rip,(void *)ep->ContextRecord->Rsp,
        (void *)ep->ExceptionRecord->ExceptionInformation[1],worker,verified);
    fflush(stderr); return EXCEPTION_CONTINUE_SEARCH;
}
int main(void)
{
    AddVectoredExceptionHandler(1,fault);
    XrDbgNativeConfig cfg={sizeof(uint64_t),capture_tls,restore_tls};
    char error[256];HANDLE threads[3];void *snapshot;size_t size;
    group=xrdbg_native_create(&cfg,error);
    if(!group){fprintf(stderr,"native backend unavailable: %s\n",error);return 77;}
    CHECK(!xrdbg_native_begin(group,5,error));
    for(unsigned i=0;i<2;i++){threads[i]=CreateThread(NULL,0,run_worker,(void *)(uintptr_t)i,0,NULL);CHECK(threads[i]);}
    wait_count(2);
    for(unsigned round=0;round<10;round++) {
        LONG64 before[2];LONG expected=verified+2;ULONGLONG end;
        depth=3;end=GetTickCount64()+5000;
        while((seen_depth[0]!=3||seen_depth[1]!=3)&&GetTickCount64()<end)Sleep(1);
        CHECK(seen_depth[0]==3&&seen_depth[1]==3);
        CHECK(xrdbg_native_begin(group,2000,error));
        CHECK(xrdbg_native_capture(group,&snapshot,&size,error));CHECK(size>4096);
        CHECK(xrdbg_native_validate(group,snapshot,size,error));
        before[0]=observed[0];before[1]=observed[1];
        xrdbg_native_end(group);depth=9;
        end=GetTickCount64()+5000;
        while((observed[0]<before[0]+10||observed[1]<before[1]+10)&&GetTickCount64()<end)Sleep(1);
        CHECK(observed[0]>=before[0]+10&&observed[1]>=before[1]+10);
        CHECK(xrdbg_native_begin(group,2000,error));
        CHECK(!xrdbg_native_validate(group,snapshot,size-1,error));
        CHECK(xrdbg_native_restore(group,snapshot,size,error));
        xrdbg_native_end(group);free(snapshot);
        end=GetTickCount64()+5000;
        while(verified<expected&&GetTickCount64()<end)Sleep(1);
        CHECK(verified==expected);
    }
    {
        ULONG previous;LONG64 frozen;ULONGLONG end;
        CHECK(ids[1]);CHECK(xrdbg_native_suspend(group,(DWORD)ids[1],&previous));CHECK(!previous);
        CHECK(xrdbg_native_begin(group,2000,error));frozen=observed[1];xrdbg_native_end(group);
        Sleep(20);CHECK(observed[1]==frozen); /* end leaves the guest logically suspended */
        CHECK(xrdbg_native_resume(group,(DWORD)ids[1],&previous));CHECK(previous==1);
        CHECK(xrdbg_native_resume(group,(DWORD)ids[1],&previous));CHECK(!previous); /* pending resume token */
        self_request=1;end=GetTickCount64()+3000;
        while(self_done<1&&GetTickCount64()<end)Sleep(1);CHECK(self_done==1);
        self_request=1;Sleep(20); /* now self-suspends without a token */
        CHECK(xrdbg_native_begin(group,2000,error));xrdbg_native_end(group);CHECK(self_done==1);
        CHECK(xrdbg_native_resume(group,(DWORD)ids[1],&previous));CHECK(previous==1);
        end=GetTickCount64()+3000;while(self_done<2&&GetTickCount64()<end)Sleep(1);CHECK(self_done==2);
    }
    CHECK(xrdbg_native_begin(group,2000,error));
    CHECK(xrdbg_native_capture(group,&snapshot,&size,error));xrdbg_native_end(group);
    stall=1;threads[2]=CreateThread(NULL,0,run_worker,(void *)2,0,NULL);CHECK(threads[2]);wait_count(3);
    CHECK(!xrdbg_native_begin(group,10,error));stall=0;Sleep(180);
    CHECK(xrdbg_native_begin(group,2000,error));
    CHECK(!xrdbg_native_validate(group,snapshot,size,error));xrdbg_native_end(group);free(snapshot);
    stop=1;CHECK(WaitForMultipleObjects(3,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<3;i++)CloseHandle(threads[i]);CHECK(xrdbg_native_count(group)==0);
    xrdbg_native_destroy(group);
    puts("PASS: native C-stack/TLS/FP restoration across recursion depths on two threads (10 rounds), logical suspend/resume tokens, topology guard, bounded pause timeout and resume");return 0;
}
