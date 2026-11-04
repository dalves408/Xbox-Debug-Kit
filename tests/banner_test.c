#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/xrdbg.c"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s (error %lu)\n",__LINE__,#x,GetLastError());exit(1);}}while(0)
static LRESULT CALLBACK fixture(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(xrdbg_window_message(h,m,w,l))return 0;
    return DefWindowProcA(h,m,w,l);
}
static void pump(void)
{
    MSG m;while(PeekMessageA(&m,NULL,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageA(&m);}
}
static void contained(HWND game)
{
    RECT b,c;POINT p={0,0};
    CHECK(xrdbg_banner_window&&IsChild(game,xrdbg_banner_window));
    CHECK(GetParent(xrdbg_banner_window)==game);
    CHECK((GetWindowLongPtrA(xrdbg_banner_window,GWL_STYLE)&WS_CHILD)!=0);
    CHECK((GetWindowLongPtrA(xrdbg_banner_window,GWL_EXSTYLE)&WS_EX_TOPMOST)==0);
    CHECK(GetClientRect(game,&c)&&ClientToScreen(game,&p)&&GetWindowRect(xrdbg_banner_window,&b));
    CHECK(b.left>=p.x&&b.top>=p.y&&b.right<=p.x+c.right&&b.bottom<=p.y+c.bottom);
}
static DWORD WINAPI request_banner(void *unused)
{
    (void)unused;
    show_banner("WORKER", "v1.0.2 (debug)", "Worker-thread request", 5000, RGB(10,200,80));
    return 0;
}
static void worker_request(void)
{
    HANDLE t=CreateThread(NULL,0,request_banner,NULL,0,NULL);
    CHECK(t);CHECK(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);CloseHandle(t);
    CHECK(xrdbg_banner_pending);
}
int main(void)
{
    s_cfg.banner_in_game=1;
    WNDCLASSA wc={0};wc.lpfnWndProc=fixture;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="XrdbgBannerFixture";
    CHECK(RegisterClassA(&wc));
    show_banner("DEBUG BUILD","Example Game v1.0.1 (debug)","Queued until the game window exists",5000,RGB(255,122,0));
    CHECK(!xrdbg_banner_window&&xrdbg_banner_pending);
    HWND h=CreateWindowA(wc.lpszClassName,"Banner test",WS_OVERLAPPEDWINDOW,100,100,656,519,NULL,NULL,wc.hInstance,NULL);CHECK(h);
    xrdbg_attach_window(h);contained(h);
    CHECK(xrdbg_banner_pending==NULL);
    CHECK(!strcmp(((struct banner_args *)GetWindowLongPtrA(xrdbg_banner_window,GWLP_USERDATA))->l2,
                  "Example Game v1.0.1 (debug)"));
    worker_request();pump();contained(h);CHECK(!xrdbg_banner_pending);
    CHECK(!strcmp(((struct banner_args *)GetWindowLongPtrA(xrdbg_banner_window,GWLP_USERDATA))->title,"WORKER"));
    SendMessageA(h,WM_SIZE,SIZE_MINIMIZED,0);pump();contained(h);
    SendMessageA(h,WM_DPICHANGED,MAKELONG(144,144),0);pump();contained(h);
    CHECK(SetWindowPos(h,NULL,240,180,350,300,SWP_NOZORDER|SWP_NOACTIVATE));pump();contained(h);
    SetWindowLongPtrA(h,GWL_STYLE,WS_POPUP|WS_CLIPCHILDREN);
    CHECK(SetWindowPos(h,NULL,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED));pump();contained(h);
    CHECK(SetWindowPos(h,NULL,80,80,30,10,SWP_NOZORDER|SWP_NOACTIVATE));pump();contained(h);
    show_banner("MARKED","v1.0.1","Replaces the previous banner within the same parent",5000,RGB(10,200,80));contained(h);
    PostMessageA(xrdbg_banner_window,WM_TIMER,1,0);pump();CHECK(!xrdbg_banner_window);
    show_banner("DEBUG BUILD","v1.0.1","Parent destruction cleans up the child",5000,RGB(255,122,0));contained(h);
    worker_request(); /* Destroy before dispatch: pending request must be freed. */
    CHECK(DestroyWindow(h));pump();CHECK(!xrdbg_banner_game&&!xrdbg_banner_window);
    CHECK(!xrdbg_banner_pending);
    show_banner("DEBUG BUILD","v1.0.2 (debug)","Reattach after parent destruction",5000,RGB(255,122,0));
    h=CreateWindowA(wc.lpszClassName,"Second parent",WS_OVERLAPPEDWINDOW,100,100,656,519,NULL,NULL,wc.hInstance,NULL);CHECK(h);
    xrdbg_attach_window(h);contained(h);CHECK(!xrdbg_banner_pending);
    CHECK(DestroyWindow(h));pump();CHECK(!xrdbg_banner_game&&!xrdbg_banner_window);
    puts("PASS: worker dispatch, queued-request cleanup, version text, reattachment, DPI,  deferred startup, child containment, move/resize/fullscreen/tiny-window bounds, replacement, expiry and parent cleanup");return 0;
}
