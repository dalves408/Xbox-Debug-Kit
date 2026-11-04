# Xbox Debug Kit (xrdbg)

A small, self-contained piece of Windows C code that turns a build of a recompiled game into one that **watches itself** and
packages what a developer needs when something goes wrong on someone else's PC. Nothing has to be installed there: no
debugger, no build tools (it uses `dbghelp.dll` and, for the zip, `tar.exe`, both part of Windows 10/11).

This is the standalone home of the reusable `xrdbg` toolkit, originally developed on the
`debug-kit` branch of [NHL Hitz Pro](https://github.com/dalves408/nhlhitzpro-recomp)
and extended during work on [NHL Hitz 2003](https://github.com/dalves408/nhlhitz2003recomp).
Use this repository's `main` branch for future integrations and updates.

The kit runs inside a **Windows build of a statically recompiled original Xbox game**.
It is a developer integration library, rather than a debugger for an Xbox console.
Game-specific register, renderer and memory hooks stay in the host game.

## Build and try the demo

Requirements: Windows 10/11, CMake 3.16 or newer, and Visual Studio 2022 with the
Desktop development with C++ workload and a Windows SDK. Python 3 is optional
and enables the collector regression test. Players need no development tools.

```powershell
git clone https://github.com/dalves408/Xbox-Debug-Kit.git
cd Xbox-Debug-Kit
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\xrdbg_demo.exe
```

In the demo, **Delete** marks a problem, **F12 deliberately crashes**, and **Esc**
quits. Startup and mark popups stay inside its window and show the configured
build version. The regression tests exercise banner layout, worker-thread
requests and cleanup, plus report collection with isolated fixture data.

## Repository contents

| Path | Purpose |
|---|---|
| `include/xrdbg.h` | Public configuration, hooks and release-build macros |
| `src/` | Crash/hang capture, logging, screenshots and optional in-window banners |
| `cmake/xrdbg.cmake` | Adds the kit to a game's debug target |
| `demo/` | Minimal Win32 integration example |
| `installer/` | Report collector and optional Inno Setup integration |
| `tests/` | Banner and report-collector regression tests |
| [CUSTOMIZING.md](CUSTOMIZING.md) | Vendoring, callbacks and game-specific integration |

For a full game integration, see `native/src/debug_report.c` and its hook sites
on the NHL Hitz Pro repository's `main` branch. The quick start below describes
the reusable API.

## What a player gets

| Situation | What happens |
|---|---|
| Game starts and closes normally | a startup debug banner appears; a log is written |
| Game crashes (access violation, `abort()`, failed assert, bad CRT parameter) | the log gets the fault, guest registers and call chain; a minidump with the guest memory is written; a zip of everything is started on the Desktop; a message box (over a fullscreen game too) says where it is |
| Game freezes (no frame for 15 s) | a dump is written once or twice, the game is left alone |
| Player presses the **mark key** (DEL) when something looks wrong | `[MARK]` line, 3 screenshots (now, +1.5 s, +4 s), a dump for the first two marks, a green banner; the game keeps running and keeps being monitored |
| Session with marks ends normally | on exit the debug folder is zipped to the Desktop and a message says where |
| Session with no mark and no crash | no exit dialog or automatic ZIP |

Files are in `%LOCALAPPDATA%\<data_folder>\debug`: `session_<date>.log` (rotated at 96 MB per half, last 6 sessions kept),
`crash_*.dmp`, `hang_*.dmp`, `mark<n>_*.bmp`, `mark<n>_*.dmp`, `running.lock` (seen at the next start: the last session did not
end cleanly, logged as `[PREV]`).

The log also carries a header (build tag, exe date, Windows, CPU, RAM, display devices, the game's settings file), a heartbeat every
5 s (frames presented, memory, handles), every controller button/stick change with a timestamp (so the steps that led to a bug can
be replayed), first-chance exceptions, and why and from where the process exited.

## Quick start (about an hour for a new game)

1. **Add the kit to the game's debug build only.** In the game's `CMakeLists.txt`:
   ```cmake
   option(MYGAME_DEBUGKIT "Debug build with the xrdbg self-monitoring kit" OFF)
   if(MYGAME_DEBUGKIT)
       include(path/to/xrdbg/cmake/xrdbg.cmake)
       xrdbg_enable(mygame_exe)          # adds src/xrdbg.c, defines XRDBG, links dbghelp/psapi/shell32, produces a PDB
   endif()
   ```
   Use a separate build directory for it. `XRDBG` is not defined in a normal build, so every `XRDBG_*` macro below is empty.
2. **Initialise first thing in `WinMain`**, before anything prints (stdout and stderr go to the log from then on):
   ```c
   #include "xrdbg.h"
   XrDbgConfig cfg = {0};
   cfg.game_name    = "My Game";
   cfg.data_folder  = "MyGame";             // %LOCALAPPDATA%\MyGame\debug
   cfg.build_tag    = "1.2.3.1-debug";
   cfg.window_class = "MyGameWindow";       // so the crash box is not hidden behind a fullscreen window (optional)
   cfg.settings_file = "settings.ini";      // copied into the log header (optional)
   cfg.frame_count  = my_frames_presented;  // uint64_t (*)(void): hang detection and heartbeat (optional but wanted)
   cfg.print_crash_context = my_crash_ctx;  // print guest registers / call chain to stderr (optional)
   cfg.guest_offset = &g_guest_mem_offset;  // host = guest + *offset, for dumps (optional)
   cfg.n_dump_ranges = 2;
   cfg.dump_ranges[0].guest_addr = 0x00000000; cfg.dump_ranges[0].size = 64u << 20;
   cfg.dump_ranges[1].guest_addr = 0x80000000; cfg.dump_ranges[1].size = 64u << 20;
   XRDBG_INIT(&cfg);
   ```
3. **Four hook points:**
   - the mark key: in the window's `WM_KEYDOWN` handler, `if (wParam == VK_DELETE) { XRDBG_MARK(); return 0; }` (don't pass the key on);
   - every place the process ends on purpose (window closed, quit menu, `HalReturnToFirmware`, `KeBugCheck`, `ExitProcess` /
     `TerminateProcess` calls): `XRDBG_NOTE_EXIT("why")` just before it;
   - the renderer's present: `while (xrdbg_capture_due(path, sizeof path)) xrdbg_write_bmp(path, pixels, w, h, pitch);` with the
     game image read back as top-down B,G,R,A pixels (see `demo/xrdbg_demo.c`; for Direct3D that is a staging-texture copy);
   - `frame_count` returns a counter the renderer increments per presented frame.
4. **Installer (optional but this is what makes it usable for players).** `installer/xrdbg.iss.inc` has the Inno Setup parts: it
   installs `collect_debug.ps1` and a Start menu "Collect debug files" shortcut, only when compiled with `/DDebugBuild=1`.
5. **A game that prints a lot** (from a GPU or audio thread): `xrdbg_enable(mygame_exe STDERR_BUFFER 65536)` makes the log
   buffered and flushed by the kit every 200 ms; see `CUSTOMIZING.md`. Integrating with an emulation toolkit that owns the
   window, renders into guest memory or has its own exit hook: `CUSTOMIZING.md` covers each case.
6. **Build, keep the symbols.** Dumps can only be read with the exact exe + PDB that produced them: archive both, with the
   installer, for every build that goes to a player.

`demo/xrdbg_demo.c` is a complete minimal user of the kit and doubles as its self-test. `cmake -S . -B build && cmake --build build
--config Release` builds it: DEL marks, F12 crashes, Esc quits. `XRDBG_SELFTEST=crash` in the environment faults 20 s after start.

## Never in a release

The kit is off unless a build asks for it: it is a separate CMake option, a separate build directory, an installer compiled with a
different version number and a `-debug` file name, and the macros vanish when `XRDBG` is not defined. Keep it that way: do not make
the option default to ON, and do not ship its installer to everyone.

## Reading what comes back

- The log first: search for `[FATAL]`, `[MARK]`, `[HANG]`, `[EXIT]`, `[PREV]`, `[EXC]`. A crash prints the fault address, access type,
  your `print_crash_context` output and a native stack.
- Dumps open in Visual Studio (File > Open) or WinDbg with the matching PDB. The guest memory ranges you named are in the dump:
  the address of a guest byte is `*guest_offset + guest_address`.
- Screenshots are plain BMP files of the game image (not of the window), taken at the mark and 1.5 s and 4 s later.

## Limits and known gaps

- Built and tested on Windows 10/11 x64. Not tested on 32-bit or Windows XP/7: on Windows before 10 1803 `tar.exe` is missing and
  the zip falls back to `collect_debug.ps1` (needs PowerShell and, for older systems, .NET 4.5 for `Compress-Archive`); with no
  PowerShell (stock XP) no zip is made and the message box says to zip the folder by hand.
- Dumps are large (the guest ranges you name plus ~10 MB; Hitz Pro: ~200 MB each, ~30 MB zipped). Only the first two marks get
  one, hang dumps are limited to two.
- The controller log uses XInput if one of `xinput1_4/1_3/9_1_0.dll` loads; otherwise there is no controller log.
- Only one process instance is supported per data folder (`running.lock`).

See [CUSTOMIZING.md](CUSTOMIZING.md) for how to adapt the kit to a specific game, and which values to tune.

## In-window banners and diagnostic context

Set `cfg.banner_in_game = 1` before `XRDBG_INIT` to keep startup and mark banners
inside the game window. After creating it, call `XRDBG_ATTACH_WINDOW(hwnd)` on
its window thread. At the start of its WndProc:

```c
if (XRDBG_WINDOW_MESSAGE(hwnd, message, wParam, lParam)) return 0;
```

Forward every message, including resize, DPI changes and destruction; continue
normal game handling when the hook returns zero. The banner scales to the client
area, follows fullscreen changes, takes no focus and expires automatically.
Startup is queued until attachment. Cross-thread requests use a bounded pending
slot and a registered message, with no heap pointers in posted messages.
The existing desktop banner remains the default for integrations without these hooks.
Child clipping requires the renderer to honor `WS_CLIPCHILDREN`; exclusive-fullscreen
or presentation paths that bypass child windows may need an equivalent renderer overlay.

Put the actual game version in `cfg.build_tag`, for example `"v1.0.2 (debug)"`.
It appears in both the startup popup and session header; source the string from
the same build configuration used by the installer. The demo shows these hooks.

The display-device inventory in the header and `computer.txt` does not identify
the adapter rendering the game. Log the adapter obtained from the created render
device after device creation (D3D11: `IDXGIDevice::GetAdapter`, then
`IDXGIAdapter::GetDesc`), including name, vendor/device IDs, dedicated memory,
feature level and creation/fallback result. Save `graphics.txt` in `xrdbg_dir()`;
the collector includes it, preferring the debug session's copy over a report in
the parent data folder. No Direct3D dependency is added to the kit.

Log video-setting changes when they are applied, with timestamp/frame number,
requested and effective resolution scale, fullscreen/aspect ratio, AA/filtering,
VSync/frame cap and texture-pack state. Use `on_mark` for a current snapshot.
This makes a slowdown mark interpretable after settings change during a match;
the startup settings file alone cannot describe it. Avoid per-frame logging.

The collector accepts `-OutputDirectory <folder>` as well as `-DataFolder` and
`-GameName`. `XRDBG_COLLECT_QUIET=1` suppresses dialogs for automated checks.
Build and run the banner regression with `ctest --test-dir build -C Release --output-on-failure`.
