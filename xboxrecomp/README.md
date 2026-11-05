# Save states for games on the xboxrecomp toolkit

F6 saves, F9 loads, in a debug build, within one run of the game. Built for finding bugs: save just before one, load, and trace the
same frames again. With no input after the save, replays have been byte-identical in the game tested so far.

Tested on one title so far, on Windows x64. Expect to test yours; see "Testing a new game".

## Add it to a game

```cmake
include(path/to/xrdbg/cmake/xrdbg.cmake)
include(path/to/xrdbg/xboxrecomp/cmake/xrx_state.cmake)
xrdbg_enable(mygame SAVE_STATES)
xrx_state_enable(mygame)
```

```c
#include "xrx_state.h"
/* After xrdbg_init */
XrxStateConfig st = { "mygame-xbox-ntsc-u", "1.0 " __DATE__ " " __TIME__, my_renderer_after_load };
xrx_state_init(&st);
/* Per presented frame: the frame as written to guest memory (bpp 4: X8R8G8B8, 2: R5G6B5) and whether the game window has focus */
xrx_state_on_flip(va, w, h, pitch, bpp);
xrx_state_keys(game_window_is_foreground);
/* From XrDbgConfig.heartbeat_extra, and on exit */
xrx_state_heartbeat(frames_presented);
xrx_state_shutdown();
```

`after_load` runs with everything parked. Use it for what your renderer keeps outside guest memory: mark textures for re-verification
and cancel deferred readbacks, or they overwrite the restored memory at the next flip. Generated code must be at
`recomp/gen/recomp_types.h` on the game's include path. Set `RECOMP_SAVE_DIR` to a scratch folder while testing.

## What is restored

| State | How |
|---|---|
| RAM, contiguous window, NV2A aperture | copied |
| Guest and device thread stacks, guest registers | native continuations (`xrdbg_native`) |
| Events, semaphores, timers, queued DPCs, allocator metadata | by stable ID; a sync object closed since the save is rebuilt |
| Guest critical sections | owner and count, written directly with everything parked |
| APU registers, interrupt line, voice runtime data | copied |
| USB controller registers, control pipes | copied |
| Render targets, texture caches | the game's `after_load` |

Not restored: guest time (it jumps forward by the gap), files the game wrote, audio already queued for the speakers, `RECOMP_PAD_SCRIPT`
position (scripted input keeps running on the wall clock), pad address/configuration, mutant ownership (warning only).
A load is refused if an open file or thread object has changed identity, or while no frames are being presented.

## Toolkit version

`generate_runtime.py` makes private copies of `kernel_bridge`, `kernel_sync`, `kernel_rtl`, `xbox_memory_layout`, `apu_core` and `ohci` with
exact-match hooks, and stops configuration with "Runtime hook mismatch" if the source differs. That failure is deliberate: it is much better
than a hook silently missing.

**Stock xboxrecomp `f0f8a46` does not match.** The hooks match from that commit plus the toolkit patch series through `0008` (see `VERSIONS.txt`).
The series is carried by the game project, not this repository. Building and running has only been done with the series through `0011`.

## Testing a new game

Matching hashes prove little. These bugs all passed them:

| Bug | What showed it |
|---|---|
| Music stopped after a load (APU state not restored) | disc reads per phase fell to almost nothing |
| Input stopped after a load (USB state not restored) | a screen stayed put through 75 s of button presses |
| Deadlock on the next scene load (locks not restored) | save and load every 4 s across a loading screen |

Test at least: a load long after the save, a load across a scene change in both directions, repeated loads of one save, and a stress
schedule across a loading screen. Then check the +600-frame screenshots, reads and audio, not only the hashes.

```
python xboxrecomp/tools/state_probe.py --exe build_debug/Release/mygame.exe --script my.pad --seconds 160 --schedule "110000:save,130000:load"
```

It uses separate settings and saves under `<build>/state-run-*` and writes `result.json` (hash checks, reads per phase) and `shots/`.
`RECOMP_AUDIO_WAV` records what the game submits. No pad input after the save tests replay: both loads should have equal hashes.

`XRDBG_STATE_CMD=<file>`: a file holding `save` or `load` acts like F6/F9, for scripted debugging.

A hang: the heartbeat logs `[STALL]` lines (locks held, by whom, where), and a hang dump opens with the exact exe + PDB.
