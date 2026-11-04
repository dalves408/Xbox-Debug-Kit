#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrdbg_state.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct Fixture {
    unsigned value, paused, resumes, restores, fatal, reject, capture_fail, pause_fail, validate_fail;
    unsigned unsupported, reenter;
    XrDbgState *engine;
    const char *path;
} Fixture;
static uint32_t capabilities(void *user, XrDbgStateOperation op, char *error)
{
    Fixture *f=user; (void)op;
    if (f->unsupported) { strcpy(error,"Execution continuation is not implemented."); return XRDBG_STATE_MEMORY; }
    return XRDBG_STATE_ALL;
}
static int pause_host(void *user, char *error)
{
    Fixture *f=user;
    if (f->pause_fail) { strcpy(error,"pause failed"); return 0; }
    CHECK(!f->paused); f->paused=1; return 1;
}
static void resume_host(void *user) { Fixture *f=user; CHECK(f->paused); f->paused=0; f->resumes++; }
static int capture(void *user, void *data, size_t capacity, size_t *used, char *error)
{
    Fixture *f=user; CHECK(f->paused); CHECK(capacity>=sizeof f->value);
    if (f->capture_fail) { strcpy(error,"capture failed"); return 0; }
    if (f->reenter) {
        CHECK(xrdbg_state_run(f->engine,XRDBG_STATE_SAVE,f->path,error)==XRDBG_STATE_BUSY);
        *error=0;
    }
    memcpy(data,&f->value,sizeof f->value); *used=sizeof f->value; return 1;
}
static int validate(void *user, const void *data, size_t size, char *error)
{
    Fixture *f=user; CHECK(f->paused); (void)data;
    if (size!=sizeof f->value || f->validate_fail) { strcpy(error,"bad payload"); return 0; } return 1;
}
static int restore(void *user, const void *data, size_t size, char *error)
{
    Fixture *f=user; CHECK(f->paused && size==sizeof f->value);
    f->restores++; memcpy(&f->value,data,size);
    if (f->reject) { f->reject--; f->value=999; strcpy(error,"injected restore failure"); return 0; } return 1;
}
static void fatal(void *user, const char *error) { Fixture *f=user; CHECK(f->paused); CHECK(*error); f->fatal++; }
static void corrupt(const char *path)
{
    FILE *file=fopen(path,"r+b"); int c;
    CHECK(file); CHECK(!fseek(file,-1,SEEK_END)); c=fgetc(file); CHECK(c!=EOF);
    CHECK(!fseek(file,-1,SEEK_END)); CHECK(fputc(c^128,file)!=EOF); CHECK(!fclose(file));
}
int main(int argc, char **argv)
{
    Fixture f={0}; XrDbgStateConfig cfg={0}; char error[256], temp[MAX_PATH], path[MAX_PATH];
    unsigned resumes;
    CHECK(GetTempPathA(sizeof temp,temp)); CHECK(GetTempFileNameA(temp,"xrs",0,path));
    CHECK(DeleteFileA(path)); f.path=path;
    cfg.game_id="test-game"; cfg.build_id="exact-build-1"; cfg.schema=1; cfg.max_payload=1024;
    cfg.user=&f; cfg.capabilities=capabilities; cfg.quiesce=pause_host; cfg.resume=resume_host;
    cfg.capture=capture; cfg.validate=validate; cfg.restore=restore; cfg.fatal=fatal;
    f.engine=xrdbg_state_create(&cfg); CHECK(f.engine);
    if (argc==2) {
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,argv[1],error)==XRDBG_STATE_INCOMPATIBLE);
        CHECK(!f.paused && !f.resumes); xrdbg_state_destroy(f.engine); return 0;
    }
    f.unsupported=1;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_UNSUPPORTED);
    CHECK(strstr(error,"continuation")); CHECK(!f.paused&&!f.resumes);
    CHECK(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES);
    f.unsupported=0; f.value=42; f.reenter=1;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_OK);
    CHECK(!f.paused && !*error);
    {
        char exe[MAX_PATH], command[MAX_PATH*2+16]; STARTUPINFOA si={0}; PROCESS_INFORMATION pi={0}; DWORD code;
        CHECK(GetModuleFileNameA(NULL,exe,sizeof exe));
        snprintf(command,sizeof command,"\"%s\" \"%s\"",exe,path); si.cb=sizeof si;
        CHECK(CreateProcessA(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
        CHECK(WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0);
        CHECK(GetExitCodeProcess(pi.hProcess,&code)); CHECK(code==0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    f.value=17; f.reenter=0;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_OK);
    CHECK(f.value==42 && !f.paused);
    /* A failed publish must retain the previous slot. */
    {
        HANDLE held=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        CHECK(held!=INVALID_HANDLE_VALUE); f.value=81;
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_IO_ERROR);
        CHECK(!f.paused); CloseHandle(held);
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_OK);
        CHECK(f.value==42);
    }
    /* Trailing bytes and a truncated envelope are refused before pause. */
    {
        FILE *file=fopen(path,"ab"); CHECK(file); CHECK(fputc(0,file)!=EOF); CHECK(!fclose(file));
        resumes=f.resumes;
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_CORRUPT);
        CHECK(f.resumes==resumes);
        file=fopen(path,"wb"); CHECK(file); CHECK(fputs("XR",file)>=0); CHECK(!fclose(file));
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_CORRUPT);
        CHECK(f.resumes==resumes);
        CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_OK);
    }
    resumes=f.resumes; corrupt(path);
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_CORRUPT);
    CHECK(f.resumes==resumes && f.value==42);
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_OK);
    cfg.build_id="different-build";
    { XrDbgState *other=xrdbg_state_create(&cfg); CHECK(other);
      CHECK(xrdbg_state_run(other,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_INCOMPATIBLE); xrdbg_state_destroy(other); }
    cfg.build_id="exact-build-1"; cfg.schema=2;
    { XrDbgState *other=xrdbg_state_create(&cfg); CHECK(other);
      CHECK(xrdbg_state_run(other,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_INCOMPATIBLE); xrdbg_state_destroy(other); }
    cfg.schema=1;
    f.value=81; f.capture_fail=1;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_HOST_ERROR); CHECK(!f.paused);
    f.capture_fail=0;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_OK); CHECK(f.value==42);
    f.validate_fail=1; resumes=f.restores;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_HOST_ERROR);
    CHECK(!f.paused && resumes==f.restores); f.validate_fail=0;
    f.value=71; f.reject=1;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_ROLLED_BACK);
    CHECK(f.value==71 && !f.paused && !f.fatal);
    f.pause_fail=1; resumes=f.resumes;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_HOST_ERROR);
    CHECK(f.resumes==resumes && !f.paused); f.pause_fail=0;
    f.reject=2;
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_POISONED);
    CHECK(f.paused && f.fatal==1);
    CHECK(xrdbg_state_run(f.engine,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_POISONED);
    xrdbg_state_destroy(f.engine); CHECK(DeleteFileA(path));
    puts("PASS: round trip, readiness gate, reentrancy, checksum, build/schema guards, capture/validation/pause failures, rollback and fatal hold");
    return 0;
}
