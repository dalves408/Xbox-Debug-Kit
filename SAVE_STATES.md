# Experimental save-state framework

Status: the shared engine, software-demo adapter and experimental Windows x64
native continuation backend work in their tests. `xboxrecomp/` uses them for working same-run
F6/F9 save states on Windows x64; see `xboxrecomp/README.md`. Games on the xboxrecomp toolkit can use `xboxrecomp/README.md` as is; another game needs its own adapter and its own testing:
hashes matching is not enough, check that streams and input still work after a load.

## Shared responsibilities

Use `xrdbg_enable(target SAVE_STATES)` and include `xrdbg_state.h`. This adds the
state engine only to a target that explicitly requests it. Existing debugger
integrations remain unchanged. The module has no dependency on a specific game,
graphics API, or Xbox memory layout.

The engine provides:

- A versioned, size-bounded file envelope with a payload/metadata corruption checksum.
- Game, exact-build, adapter-schema and process-instance compatibility checks.
- One synchronous operation at a time, with reentrant/concurrent calls rejected.
- A complete-subsystem readiness gate before any file access or runtime pause.
- Host pause/capture/validate/restore/resume callbacks.
- A rollback capture before loading; a failed restore tries to reinstate that capture.
- A poisoned-state result and fatal callback if rollback also fails. The engine
  leaves the runtime paused in this case; the host must report/terminate it.
- Writes to a unique sibling temporary file, flush, then replacement of the slot.
  Capture/write failures do not truncate a previous slot.

States currently work only within the **same running process**. They are not
portable between builds, launches, machines or games. The checksum detects
accidental damage; it is not authentication. Accept only trusted local states,
and independently validate the complete payload before interpreting it.
Use a separate save-data directory during development: restoring file offsets
does not undo external file writes or changes made by another process.

The engine copies the configuration strings. Other callback data must outlive
the engine. Destroy it only after its coordinator has stopped; do not destroy it
concurrently with an operation. The error buffer passed to each callback has
`XRDBG_STATE_ERROR_SIZE` bytes. Return zero and explain failure there. Callbacks
must return normally; access violations and nonlocal jumps are not recoverable
state-operation errors.

## Adapter contract

1. Give the adapter stable game/region and exact executable/runtime build IDs,
   a payload schema, and a bounded payload capacity. Change the schema when
   serialization changes; identify the executable and runtime, not just a release label.
2. Inventory eight required subsystems: memory, execution, kernel objects,
   GPU, audio, files, clock and input. `capabilities` may return a bit only when
   that subsystem really can be restored. It must not mutate runtime state.
   An absent device can count as supported if its absence is part of the adapter.
3. Run requests on a dedicated coordinator or a proven simulation safe point.
   A GPU callback cannot wait for the GPU worker to pause itself. A WndProc
   cannot block on work that needs its message pump. Queue keyboard requests.
4. `quiesce` must bring every mutator to a cooperative safe boundary, drain or
   serialize outstanding work, and have a bounded timeout. If it fails it must
   unwind its own partial pauses. The engine does not resume a failed pause.
   Do not arbitrarily suspend threads that may own heap, file or driver locks.
5. `capture` serializes the entire resumable simulation while quiesced. Use
   offsets/IDs and lengths, not host pointers, COM objects, mutexes or HANDLEs.
   Prefer tagged, versioned sections with strict size/count bounds.
6. `validate` checks the whole payload without changing runtime state. It runs
   while paused. Validate the captured rollback image too.
7. `restore` reconstructs host resources and logical state. It may fail after
   mutation only if the previous captured payload can recover all changes.
   Irreversible host actions require an additional prepare/commit design in the
   adapter; generic memory rollback cannot undo writes to the outside world.
8. `resume` releases the pause after success or a recoverable failure. `fatal`
   must keep a poisoned runtime paused and report/terminate it.

The framework cannot prove a provider's capability claims, pause discipline,
payload completeness or rollback correctness. Test those in the host runtime.
The software demo is deliberately small: its simulation uses a frame number
and an 8 MiB fake guest buffer; graphics are regenerated from that state. It
has no guest threads, audio voices or open game files. Its F6/F9 example is not
evidence that native Xbox worker stacks can be restored.

## xboxrecomp integration

The opt-in build of a game on the toolkit uses:

```powershell
cmake -S . -B build_debug -DMYGAME_DEBUGKIT=ON -DMYGAME_SAVE_STATES=ON
cmake --build build_debug --config Release --parallel 2
```

`xboxrecomp/src/xrx_state.c` is the adapter, shared by games on that toolkit. F6/F9 requests are edge-triggered while
the game has focus and queued to a coordinator; held keys do not repeat. Results
are logged with `[STATE]` and shown with `xrdbg_banner`. Sections beyond guest
memory: native continuations, kernel objects, allocator metadata, APU and USB device
state. The rest of this section records the design reasoning from before that work.

Evidence from this runtime:

- `src/recomp/gen/recomp_types.h`: guest return addresses do not drive execution;
  generated functions use native C call/return, with some registers in TLS and
  other state in function locals. Xbox RAM plus a guest instruction address is
  insufficient to resume them.
- Toolkit `src/kernel/kernel_bridge.c`, `bridge_thread_main` and
  `bridge_spawn_thread`: guest workers use `CreateThread`, native stacks, TLS,
  thread objects, a guest-CPU gate and deferred suspension.
- Toolkit `src/kernel/nv2a_pb_exec.c`: decoder registers, queued work and device
  state also live in host statics. Waiting for the GPU alone does not stop guest
  producers or the timer/ack/audio workers.
- Game `src/gpu_backend.c`: render/depth targets, content-hashed textures,
  deferred readbacks, ring buffers and COM objects are outside Xbox RAM.
- Kernel memory allocation, physical naming, synchronization objects, open
  files, APU/DirectSound voices, clock and input script positions also need
  serializers or deterministic reconstruction.

Required implementation order:

1. Add cooperative runtime-wide quiescence with worker registration, bounded
   acknowledgements and tests for pause/unpause during menus and fights.
2. Implement resumable guest execution continuations. A Windows x64 native-stack backend now exists for the same-session case;
   complete its runtime TLS/host-object coverage, or use an explicit recompiler
   continuation format for a portable implementation. Suspending native threads and copying their stacks
   does not rewind Windows wait objects, TLS, resources or host-side lifetimes.
3. Serialize guest mappings, heaps/allocators and logical kernel objects using
   stable IDs, including guest stack/TLS mappings and all worker continuations.
4. Serialize GPU and audio logical state, reconcile deferred readbacks, and
   rebuild/invalidate host caches and device resources before resuming.
5. Add file-offset/async-I/O, clock/timer and input-state serializers. Keep
   changes to normal persistent game saves separate from debug checkpoints.
6. Enable capability bits only after each implementation passes save/load
   tests. Exercise repeated loads, scene transitions, thread/object lifetime
   changes, active music and save-file activity, then permit gameplay use.

None of these missing runtime facilities is supplied by a crash minidump. The
shared framework is the reusable orchestration layer for them.

## Native continuation backend: Windows x64 experiment

`include/xrdbg_native.h` and `src/xrdbg_native.c` provide a cooperating worker
group. `src/xrdbg_stack_x64.asm` moves a worker onto a separate, fully committed
parking stack. Its original native C stack is then inactive and can be copied.
A saved Windows CONTEXT and adapter-selected TLS restore its native call frames
on that same thread. The implementation uses [RtlCaptureContext](https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-rtlcapturecontext)
and [RtlRestoreContext](https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-rtlrestorecontext).

```cmake
enable_language(ASM_MASM)  # at the consuming project's top level
xrdbg_enable(game SAVE_STATES)
xrdbg_native_enable(game)
```

Register each worker on itself, call `xrdbg_native_checkpoint` only at explicit
lock-free boundaries, and unregister after its last simulation mutation. Keep
its group alive until all workers have stopped and joined. A coordinator calls
`xrdbg_native_begin` with a bounded timeout, then capture/validate/restore, then
`xrdbg_native_end`. A timeout cancels the pause and releases parked workers.
Names and adapter-defined location codes help identify an uncooperative thread.
Logical guest suspend/resume counts keep dormant workers on their parking stacks;
a resume that arrives before a self-suspend leaves one token. Ending a checkpoint
pause does not wake a guest that is still logically suspended.

Native snapshots require the same process, group identity, worker membership,
thread IDs, stack mappings, executable and adapter schema. Validate the whole
snapshot before any writes. For persisted data, also use the outer state engine's
build/session/integrity envelope. Payloads intentionally contain process-local
native contexts and stack pointers; logical device sections should still use IDs
and offsets. The native backend cannot restore Windows handles or object lifetimes.

This is currently limited to MSVC Windows x64 C code using the baseline x64
floating-point/SSE state. Fibers, AVX/XSTATE restoration, C++ exception unwinding,
changing thread topology and CET user shadow stacks are unsupported. Creation
rejects a process with user shadow stacks enabled; it does not disable security
features. The parking trampoline changes thread stack bounds while active.
Exceptions or stack overflow on that stack are fatal; do not unwind across it.
No Linux or cross-process continuation support is claimed.

The native regression restores C locals, floating-point locals and TLS across
recursion depths 3 and 9 on two threads for ten rounds. It also checks logical
suspension, pending-resume tokens, malformed snapshots, changed topology and
pause-timeout recovery. This proves the primitive in that test, not every host
runtime's locking and lifetime rules.

### xboxrecomp runtime integration

The experimental configuration generates private copies of `kernel_bridge.c`,
`kernel_sync.c`, `xbox_memory_layout.c`, `apu_core.c` and `ohci.c` under the build
directory. Exact-match source hooks fail configuration if the toolkit revision
changes. The shared toolkit checkout and normal builds are not edited.

Guest registers, x87/SSE emulation values and generated-code TLS are captured.
Guest checkpoints are skipped while a guest owns its CPU gate or has raised IRQL.
Blocking guest waits poll for a pause without changing APC/WaitAll/failure status
semantics. Registered guest suspension uses the parking mechanism, retaining the
original startup fallback for a thread not registered yet.

Pausing occurs in two stages: first guest workers while devices can still finish
their work, then GPU executor, APU frame, USB OHCI, kernel timer and vblank ack
workers at unlocked loop boundaries. This avoids stopping a device that a guest
still needs in order to reach a safe point. Device callbacks cannot park through
a nested guest kernel hook while holding an outer device lock.

**F7 is a diagnostic probe**, not a save-state command. It captures and validates
the parked native frames in memory, then resumes. Developers can set
`XRDBG_STATE_NATIVE_PROBE_MS=12000,45000,90000` for timed probes after startup and
`XRDBG_STATE_NATIVE_PROBE_RESTORE=1` to echo the just-captured native frames back
before resuming. Echo restoration has no intervening gameplay and does not rewind
memory, resources, a match or audio playback.

The probes have parked seven guest and five core device workers in real game
runs and resumed frame progress. Remaining work includes complete host/kernel
TLS coverage, mapping/allocator/object state, all asynchronous callbacks, GPU
resource/pipeline state, APU/output queue state, persistent file side effects,
and clock/input serialization. A pause of these twelve workers alone is not
proof that every OS/driver callback is quiescent. Keep capability bits disabled
until a full logical-state provider and repeated gameplay rewinds are verified.
