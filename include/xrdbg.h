/* xrdbg: self-monitoring debug kit for statically recompiled Xbox games. See README.md.
 *
 * Add src/xrdbg.c to the game's debug build only (the CMake helper in cmake/xrdbg.cmake does that and defines XRDBG),
 * call xrdbg_init() first thing in WinMain, and hook up the five points below. With XRDBG undefined every XRDBG_* macro
 * is empty, so the hooks can stay in release code. */
#ifndef XRDBG_H
#define XRDBG_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XRDBG_MAX_RANGES 8

typedef struct XrDbgConfig {
    const char *game_name;       /* shown in the log header and message boxes, e.g. "NHL Hitz Pro" */
    const char *data_folder;     /* folder under %LOCALAPPDATA%, e.g. "NHLHitzPro"; files go to <it>\debug, zips are named <it>-debug-... */
    const char *build_tag;       /* e.g. "1.0.7.6-debug": log header and the start-up banner */
    const char *window_class;    /* the game window's class: minimised before a crash box so the box is not hidden behind it (NULL: skip) */
    const char *settings_file;   /* file in <data_folder> whose text is copied into the log header, e.g. "settings.ini" (NULL: none) */
    const char *mark_key_name;   /* what the banner calls the mark key; default "DEL" */
    int mark_dumps;              /* how many marks also write a dump (they are large); default 2 */

    uint64_t (*frame_count)(void);                      /* frames presented so far: hang detection, heartbeat, mark lines (NULL: none) */
    void (*print_crash_context)(PEXCEPTION_POINTERS);   /* print guest registers / call chain to stderr at a crash (NULL: none) */

    /* Optional extension points: print extra text (to stderr, lines of its own) into the log. */
    void (*header_extra)(void);      /* once, at the end of the header: emulator settings, renderer in use, ... */
    void (*heartbeat_extra)(void);   /* every 5 s: queue depths, allocator use, whatever the game's own bugs need */
    void (*on_mark)(int mark_no);    /* on each mark key press, from the key handler's thread: a state dump for the glitch */

    /* Guest memory to include in every dump: host address = guest_addr + *guest_offset (e.g. an emulator's g_xbox_mem_offset). */
    const ptrdiff_t *guest_offset;
    int n_dump_ranges;
    struct { uint64_t guest_addr; uint64_t size; } dump_ranges[XRDBG_MAX_RANGES];
} XrDbgConfig;

/* 1. First thing in WinMain, before anything prints: stdout and stderr go to the log file from here on. */
void xrdbg_init(const XrDbgConfig *cfg);

/* 2. Before the process ends on purpose (window closed, quit chosen, HalReturnToFirmware/KeBugCheck, ExitProcess paths):
 *    logs why, and if marks were made and nothing crashed, zips the debug folder to the Desktop and tells the player. */
void xrdbg_note_exit(const char *why);

/* 3. On the mark key (a WM_KEYDOWN handler; do not pass the key on to the game). */
void xrdbg_mark(void);

/* 4. Once per presented frame, in the renderer: if this returns 1, write the game image to <path>.bmp. xrdbg_write_bmp()
 *    does the file from top-down B,G,R,A pixels. Call it in a loop: several captures can be due. */
int xrdbg_capture_due(char *path, size_t n);
int xrdbg_write_bmp(const char *path_no_ext, const void *bgra, int w, int h, int pitch);

/* 5. Where the files go (%LOCALAPPDATA%\<data_folder>\debug), for anything else the game wants to put there. */
const char *xrdbg_dir(void);

/* Developer self-test: XRDBG_SELFTEST=crash in the environment makes the process fault 20 s after xrdbg_init. */

#ifdef __cplusplus
}
#endif

#ifdef XRDBG
#define XRDBG_INIT(cfg)        xrdbg_init(cfg)
#define XRDBG_NOTE_EXIT(why)   xrdbg_note_exit(why)
#define XRDBG_MARK()           xrdbg_mark()
#else
#define XRDBG_INIT(cfg)        ((void)0)
#define XRDBG_NOTE_EXIT(why)   ((void)0)
#define XRDBG_MARK()           ((void)0)
#endif

#endif
