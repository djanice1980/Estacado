# M4 runtime foundation and first execution

## Implemented and verified

- The native target is `build/runtime/TheDarkness.exe`.
- It parses the local `default.xex`, verifies entry `0x828AA3E8`, maps all 17 image sections, preserves a zero-filled virtual tail in `.reloc`, registers 32,433 generated functions, and invokes `_xstart`.
- Guest memory is a page-file-backed Windows mapping with the Xbox 360 XEX and physical alias ranges represented directly. This replaces the earlier flat reservation that faulted when guest startup dereferenced `0x93010000`.
- All unresolved imports are generated as explicit throwing traps. There are 198 such traps in the current runtime build.
- Verified implementations now include `KeQueryPerformanceFrequency`, `MmQueryStatistics`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, guest TLS, typed unnamed events/semaphores, immediate-start `ExCreateThread`, object reference/dereference, affinity/priority state, critical sections, delay, FPU mode, and single-object waits.

## First-execution evidence

`logs/first_execution.log` proves the sequence:

1. `HOST_START`, XEX parse, guest memory map, image map, and dispatch registration.
2. `XEX_ENTRY_ADDRESS=0x828AA3E8` and `FIRST_GENERATED_FUNCTION=_xstart`.
3. Twelve calls to `KeQueryPerformanceFrequency`, each returning `50,000,000`.
4. `MmQueryStatistics` returns a coherent 104-byte big-endian result sourced from tracked physical allocations.
5. The game creates eleven events, one semaphore, and starts its first worker through `0x828ADA98 → 0x821FBB00`.

## Current hard runtime boundary

The coupled creator/worker object and critical-section boundary is resolved.
The latest bounded run creates four workers, each resolves its current-thread
pseudo-handle, clears its per-thread FPU exception mode, and waits on startup
semaphore `0xD`. The semaphore has verified initial count zero and no release
was observed in this execution window, so the runtime blocks the workers
rather than manufacturing work. See `docs/OBJECT_MANAGER.md`,
`docs/THREADING_MODEL.md`, and `docs/SYNCHRONIZATION.md`.

## Reference evidence

- Xenia's guest memory map defines the `0x90000000` XEX 4 KiB range and its backing-store aliasing: <https://github.com/xenia-project/xenia/blob/master/src/xenia/memory.cc>.
- Xenia initializes the 360 guest clock at 50 MHz and returns that frequency from `KeQueryPerformanceFrequency`: <https://github.com/xenia-project/xenia/blob/master/src/xenia/emulator.cc> and <https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc>.
- Its `MmQueryStatistics` implementation identifies the requested 104-byte structure but comments that its concrete statistics are mostly guessed: <https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc>.
