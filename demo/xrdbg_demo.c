/* A tiny Win32 program that uses xrdbg the way a recompiled game would: it is also the kit's self-test.
 *
 *   window with a moving square (the "game image"), 60 frames a second
 *   DEL       marks a glitch (screenshots, log line, dump)
 *   F12       crashes on purpose (write to address 0)
 *   Esc       quits through xrdbg_note_exit (zips the debug folder if anything was marked)
 *   XRDBG_SELFTEST=crash in the environment crashes 20 s after start without a key press */
#include "xrdbg.h"
#include <stdio.h>
#include <stdlib.h>

static volatile uint64_t g_frames;
static uint32_t g_pixels[320 * 240];            /* the "game image", B,G,R,A */
static ptrdiff_t g_fake_guest_offset;
static uint8_t *g_fake_guest;                   /* 8 MB standing in for the emulated Xbox RAM */

static uint64_t frame_count(void) { return g_frames; }

static void crash_context(PEXCEPTION_POINTERS ep)
{
    fprintf(stderr, "  demo crash context: fault at %p (a real game prints guest registers and its call chain here)\n",
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL);
}

static void header_extra(void) { fprintf(stderr, "demo renderer : software, 320x240\n"); }
static void heartbeat_extra(void) { fprintf(stderr, "[HB-demo] square at x=%u\n", (unsigned)(g_frames % 280)); }
static void on_mark(int n) { fprintf(stderr, "[MARK-demo] state at mark #%d: frame %llu\n", n, (unsigned long long)g_frames); }

static void draw_frame(void)
{
    int x, y, sq = (int)(g_frames % 280);
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            g_pixels[y * 320 + x] = (x >= sq && x < sq + 40 && y >= 100 && y < 140) ? 0x00FF7A00u : 0x000E1628u;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_DELETE && !(l & (1L << 30))) { XRDBG_MARK(); return 0; }
        if (w == VK_F12) *(volatile int *)0 = 1;
        if (w == VK_ESCAPE) { XRDBG_NOTE_EXIT("quit with Esc"); ExitProcess(0); }
        break;
    case WM_CLOSE:
        XRDBG_NOTE_EXIT("the window was closed");
        ExitProcess(0);
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    XrDbgConfig cfg;
    WNDCLASSA wc;
    HWND hw;
    MSG msg;
    (void)hp; (void)cmd; (void)show;

    g_fake_guest = (uint8_t *)VirtualAlloc(NULL, 8u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_fake_guest_offset = (ptrdiff_t)g_fake_guest;          /* "guest address 0" is the start of the block */
    memset(&cfg, 0, sizeof cfg);
    cfg.game_name = "xrdbg demo";
    cfg.data_folder = "xrdbg-demo";
    cfg.build_tag = "demo";
    cfg.window_class = "XrdbgDemo";
    cfg.frame_count = frame_count;
    cfg.print_crash_context = crash_context;
    cfg.header_extra = header_extra;
    cfg.heartbeat_extra = heartbeat_extra;
    cfg.on_mark = on_mark;
    cfg.guest_offset = &g_fake_guest_offset;
    cfg.n_dump_ranges = 1;
    cfg.dump_ranges[0].guest_addr = 0;
    cfg.dump_ranges[0].size = 8u << 20;
    XRDBG_INIT(&cfg);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "XrdbgDemo";
    RegisterClassA(&wc);
    hw = CreateWindowExA(0, "XrdbgDemo", "xrdbg demo (DEL = mark, F12 = crash, Esc = quit)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         100, 100, 656, 519, NULL, NULL, hi, NULL);
    printf("demo started, window %p\n", (void *)hw);
    for (;;) {
        char cap[MAX_PATH];
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        draw_frame();
        g_frames++;
#ifdef XRDBG
        while (xrdbg_capture_due(cap, sizeof cap))
            xrdbg_write_bmp(cap, g_pixels, 320, 240, 320 * 4);
#endif
        Sleep(16);
    }
}
