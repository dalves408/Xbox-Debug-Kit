# Making xrdbg specific to the game you are working on

The kit is deliberately two layers. Keep them apart and updating from this branch stays easy.

1. **Your glue**: one small file in your own project (call it `mygame_debug.c`) that fills in `XrDbgConfig` and holds the few
   callbacks. Everything game-specific goes here: guest registers, memory map, frame counter, extra state.
2. **The kit**: `src/xrdbg.c` and `include/xrdbg.h`. Treat them as vendored: avoid editing them in your project, so a later
   `git merge`/`git subtree pull` of this branch applies cleanly. If you must change something, change it here first (the branch
   is the shared source) or note the change in a comment starting `PROJECT OVERRIDE:` like the NHL project does for toolkit files.

## Pulling the kit into a project

Any of these works; pick the one your project already uses for the toolkit:

```bash
# as a subtree (code is copied into your repository, history optional):
git remote add xrdbg https://github.com/dalves408/nhlhitzpro-recomp.git
git fetch xrdbg debug-kit
git subtree add --prefix=tools/xrdbg xrdbg debug-kit --squash
git subtree pull --prefix=tools/xrdbg xrdbg debug-kit --squash      # later, to update

# or just the files:
git fetch xrdbg debug-kit && git checkout xrdbg/debug-kit -- include src cmake installer
```

## Where each game-specific thing goes

| Your game | What to do in the glue |
|---|---|
| Different memory map (RAM size, contiguous/GPU window, other emulated ranges) | `dump_ranges[]`: guest address and size of each range worth having in a dump, and `guest_offset` pointing at the variable that holds *host = guest + offset*. Name only what you will actually read: every range adds to every dump. If your emulator maps guest memory differently per range, set `guest_offset` to the main one and add ranges the same way; ranges that are not committed are skipped quietly. |
| Guest CPU state in a crash (registers, call chain, current guest function) | `print_crash_context`: print it to `stderr`. It runs inside the crash handler on the faulting thread (stack may be nearly gone, heap may be damaged: keep it to `fprintf` of plain memory reads, no allocation, no locks). The NHL project prints its recompiled register globals and scans the guest stack for return addresses. |
| What "a frame" is | `frame_count`: any counter that increases once per presented frame. It drives hang detection, the heartbeat and the mark lines. If your game has no renderer yet (headless test), leave it `NULL`; hang detection is then off. |
| Another renderer (Direct3D 8/9/11, OpenGL, software) | Only the screenshot call. Per presented frame: `while (xrdbg_capture_due(path, sizeof path)) { read the game image back; xrdbg_write_bmp(path, bgra_pixels, w, h, pitch); }`. For Direct3D that is `CopyResource` into a staging texture, `Map`, pass the mapped pointer and row pitch. OpenGL: `glReadPixels` as BGRA, flipped. Capture the **game image** (the emulated screen), not the window, so scaling and overlays do not matter. |
| Another input source for marking | The kit does not read keys itself. Call `xrdbg_mark()` from anywhere: a key, a controller combo, a menu entry. Set `mark_key_name` so the banner tells the player what to press. |
| Another way the process ends | Call `xrdbg_note_exit("what happened")` before it. Missing one means a normal session would not zip marks on that path. Grep your code for `ExitProcess`, `TerminateProcess`, `exit(`, `_exit(`, window close, quit menus, and the kernel's shutdown/bugcheck exports. |
| Game-specific state worth logging | `header_extra` (once, end of the log header: emulator version, renderer, settings), `heartbeat_extra` (every 5 s: queue depths, allocator use, loader state), `on_mark` (at each mark, from the key handler's thread: dump whatever describes "what is on screen": current scene, draw counts, loaded asset names). Print to `stderr`; lines should be self-explanatory and start with a tag you can grep. |
| Your game prints its own diagnostics | Nothing to do: `xrdbg_init` redirects `stdout` and `stderr` to the log, so existing `fprintf(stderr, ...)` output is captured (and rotated). Call `xrdbg_init` before the first print. |
| A settings file you want in every report | `settings_file` (name inside `%LOCALAPPDATA%\<data_folder>`); its first 2 KB go in the header. The collect script also adds every `*.ini` in that folder to the zip. |
| Names, folder and zip names | `game_name`, `data_folder`, `build_tag`. The zip is `<data_folder>-debug-<crash\|marks>-<date>.zip`. |
| Window not found / crash box hidden | `window_class` must be the exact class name your window was registered with (`RegisterClass`). If it is wrong nothing breaks; the crash box just cannot first push the game window out of the way. |
| A game that logs a lot | By default the log is unbuffered: every `fprintf` is a `WriteFile` under the CRT stream lock. If the game prints from a thread it waits on (a GPU/pushbuffer executor, an audio thread), that lock can cost a large share of the frame. Build with a buffer instead: `xrdbg_enable(mygame_exe STDERR_BUFFER 65536)` (or define `XRDBG_STDERR_BUFFER`). Both `stderr` and `stdout` are then buffered, and the kit flushes them itself every 200 ms and on crash/exit, so at most the last ~200 ms are lost if the process is killed outright. |
| Lines from different threads mixed together in the log | Unbuffered, the CRT writes one `fprintf` to a file in several pieces, so two threads printing at the same moment interleave mid-line. No bytes are lost: the log handle is opened for atomic append (`FILE_APPEND_DATA`), shared by `stdout` and `stderr`. If the game prints from several threads at once and you need whole lines, use the buffered log (row above): each stream then reaches the file in whole buffers, flushed between calls. |
| The game already sets up `stdout`/`stderr` | A game that calls `setvbuf` on them (or starts its own flusher) must skip that in the debug flavour: the C library does not allow re-buffering a stream after it has been written to, and `xrdbg_init` has written the log header by then. Wrap the game's own setup in `#ifndef XRDBG`. |
| The window procedure is not yours (an emulation toolkit owns the window) | Poll the key instead: once per presented frame, `GetAsyncKeyState(VK_DELETE)` on its rising edge, and only while `GetForegroundWindow()` is the game window (compare `GetClassNameA` with your `window_class`), so a DEL typed in another program does not mark. |
| The renderer copies each frame back into guest memory (common in xboxrecomp-style NV2A emulation) | The screenshot can come straight from guest memory: the NV2A CRTC scan-out register `PCRTC_START` (register offset `0x600800`) holds the physical address of the frame being shown; with a 1:1 contiguous window that is guest VA `0x80000000 | address`. Hand that buffer (X8R8G8B8, i.e. B,G,R,X bytes, pitch = width x 4) to `xrdbg_write_bmp`, right after the renderer's copy-back for the frame. No extra GPU readback is needed. |
| An exit hook API in the toolkit | If your runtime already has a "run before ExitProcess" hook (xboxrecomp: `recomp_exit_hook_set` / `recomp_exit_hook_run`), register `xrdbg_note_exit` through it rather than editing each exit path. Check two things: that the hook API keeps more than one hook (an older xboxrecomp kept one, and its missing-file report registered lazily and replaced earlier hooks), and that the kernel's exit paths (`KeBugCheck`, `KeBugCheckEx`, `HalReturnToFirmware`, `HalInitiateShutdown`) run it too, not only the window close. |

## Tuning knobs (constants in `src/xrdbg.c`)

| Symbol or value | Default | Meaning |
|---|---|---|
| `LOG_HALF_BYTES` | 96 MB | the log is two halves of this size; the older half is overwritten |
| `KEEP_SESSIONS` | 6 | session logs kept (older ones deleted at start) |
| `t - last_move > 15000` (`monitor_thread`) | 15 s | no presented frame for this long is a hang |
| `hang_dumps < 2` | 2 | hang dumps per run |
| `t - last_hb >= 5000` | 5 s | heartbeat interval |
| `delays[3] = { 0, 1500, 4000 }` (`xrdbg_mark`) | now, +1.5 s, +4 s | when the three screenshots are taken after a mark; lengthen for slow effects |
| `t - last < 700` (`xrdbg_mark`) | 0.7 s | marks closer together count once |
| `MARK_DUMPS_DEFAULT` / `mark_dumps` | 2 | marks that also write a dump |
| banner durations in `xrdbg_init` / `xrdbg_mark` | 5 s / 2.8 s | start-up banner and "MARKED" banner |
| `WaitForSingleObject(zp, 180000)` | 3 min | how long the exit zip is waited for |
| `now_s() > 20.0` (selftest) | 20 s | `XRDBG_SELFTEST=crash` delay |
| `XRDBG_STDERR_BUFFER` (compile definition) | unset = unbuffered | log buffer size in bytes (stderr and stdout); set through `xrdbg_enable(... STDERR_BUFFER n)` |
| `Sleep(200)` (`log_flusher_thread`) | 200 ms | how often a buffered log is flushed |

## Things worth checking when you adopt it

- **Is `xrdbg_init` really first?** Anything printed or any `freopen` of `stdout`/`stderr` before it is lost or fights it. A release build that sends output to `NUL` needs that code skipped in the debug flavour (the NHL project does this in its `release_init`).
- **Does the game strip or ignore the environment?** The kit itself needs no environment variables; they are only developer switches (`XRDBG_SELFTEST`, `XRDBG_COLLECT_QUIET`).
- **Symbols**: `xrdbg_init` initialises dbghelp (`SymInitialize`) so `[STACK]` and `[EXC]` lines carry function names. If the game calls `SymInitialize` itself later, that call returns FALSE (already initialised) and is harmless; do not call `SymCleanup` while the kit is running.
- **Exception handlers of your own**: the kit installs `SetUnhandledExceptionFilter` plus a first-chance logger and CRT abort/invalid-parameter/purecall handlers. If your emulator also installs a vectored handler that handles access violations for emulated hardware, that is fine (first-chance AVs are not logged); if it calls `SetUnhandledExceptionFilter` itself, install the kit after it.
- **Dump size**: before naming a range, check it is committed memory and how big your real dump gets. A 64 MB RAM image plus a 64 MB GPU window gave ~200 MB per dump and ~30 MB zipped.
- **A new Windows version for the target PCs** (XP, 7): see "Limits" in the README; the zip may need a manual step.
- **Prove it before you ship it**: build the debug flavour, run the game, press the mark key, and force a crash (`XRDBG_SELFTEST=crash`). Open the dump in Visual Studio with the PDB and confirm the guest ranges are in it. The demo app shows each of these working in isolation.

## Reference integration

The NHL Hitz Pro project (branch `main`, `native/`) is the example of a full integration: `src/debug_report.c` (the same code before it was
made configurable), the five hook lines in `src/backend_d3d11.c` (frame counter, DEL key, screenshot after the frame is resolved,
exit notes), `overrides/kernel_hal.c` (exit notes on shutdown/bugcheck), `src/main.c` (crash context printer and init call),
`installer/nhl_hitz_pro.iss` (the debug-only parts) and `DEBUG-BUILD.md`.
