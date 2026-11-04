/* Exercise the actual software-demo provider, including its full fake RAM. */
#define WinMain xrdbg_demo_entry
#include "../demo/xrdbg_demo.c"
#undef WinMain
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void)
{
    XrDbgStateConfig cfg={0};
    char temp[MAX_PATH], path[MAX_PATH], error[XRDBG_STATE_ERROR_SIZE];
    size_t i;
    CHECK(GetTempPathA(sizeof temp,temp)); CHECK(GetTempFileNameA(temp,"xrd",0,path)); CHECK(DeleteFileA(path));
    g_fake_guest=VirtualAlloc(NULL,8u<<20,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); CHECK(g_fake_guest);
    cfg.game_id="demo-adapter-test"; cfg.build_id="demo-adapter-test-1"; cfg.schema=1;
    cfg.max_payload=sizeof(uint64_t)+(8u<<20);
    cfg.capabilities=state_capabilities; cfg.quiesce=state_pause; cfg.resume=state_resume;
    cfg.capture=state_capture; cfg.validate=state_validate; cfg.restore=state_restore; cfg.fatal=state_fatal;
    g_state=xrdbg_state_create(&cfg); CHECK(g_state);
    for (unsigned round=0; round<3; round++) {
        g_frames=42+round;
        memset(g_fake_guest,17+round,8u<<20);
        CHECK(xrdbg_state_run(g_state,XRDBG_STATE_SAVE,path,error)==XRDBG_STATE_OK);
        g_frames=999; memset(g_fake_guest,99,8u<<20);
        CHECK(xrdbg_state_run(g_state,XRDBG_STATE_LOAD,path,error)==XRDBG_STATE_OK);
        CHECK(g_frames==42+round && !g_state_paused);
        for (i=0; i<(8u<<20); i++) CHECK(g_fake_guest[i]==17+round);
    }
    xrdbg_state_destroy(g_state); CHECK(DeleteFileA(path)); CHECK(VirtualFree(g_fake_guest,0,MEM_RELEASE));
    puts("PASS: software-demo adapter repeatedly restores frame position and every byte of 8 MiB fake guest RAM");
    return 0;
}
