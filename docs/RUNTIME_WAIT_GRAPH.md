# M4D startup wait graph

Evidence: `logs/thread_liveness_snapshot.log` (bounded probe).

```text
Thread 1 (creator): RUNNING
  -> returns from all four ExCreateThread calls
  -> repeatedly calls KeDelayExecutionThread through 0x828AC000

Thread 2: WAITING -> semaphore handle 0xD (infinite timeout)
Thread 3: WAITING -> semaphore handle 0xD (infinite timeout)
Thread 4: WAITING -> semaphore handle 0xD (infinite timeout)
Thread 5: WAITING -> semaphore handle 0xD (infinite timeout)
```

The creator is not replaced by a child context and is not blocked. The worker
starts are independent host threads. No runnable guest worker was observed
without host execution.

## Verified missing edge and repair

The creator's loop is `sub_821F0B20`, not the generic delay helper. It polls
each worker context's `+0x34` readiness word. The first observed word was
`0xA0165FAC` and was initially zero. The worker callback `sub_821F11A0` writes
that word from `sub_828A7CF8`, which loads `r13+0x100` then `+0x14C` (the guest
thread ID).

The runtime previously initialized main `r13` to zero and copied that zero
value to every child. It therefore supplied no PCR/TEB and workers wrote zero
as their readiness value. The repair creates a distinct guest PCR/TEB and TLS
base for each thread; `r13+0x100` now points to the per-thread state and
`+0x14C` contains its guest ID. The live trace then observed IDs 3, 4, and 5
at later readiness words, and thread 1 progressed to create thread 6 without
any synthetic semaphore operation.

## Static release sites

All reached generated calls funnel through `sub_828A9E80`, the title wrapper
for `NtReleaseSemaphore`.

| Guest call address | Caller region | Release count source | Dynamic state |
| --- | --- | --- | --- |
| `0x821F0FA0` | task-queue publication path | `*(r31 + 52)` | Not reached |
| `0x821F10C4` | task-queue completion path | `*(r31 + 52) - 1` | Not reached |
| `0x821F1530` | engine service path | constant `1` | Not reached |
| `0x825C3790` | later engine service path | constant `1` | Not reached |

None is proven to target handle `0xD`: each obtains its handle from a guest
structure field. The startup semaphore producer remains unidentified.

## Resolved producer/consumer evidence

With the raw cache absent, the unmodified title takes its error-2 fallback,
creates threads 7–9, and calls `NtReleaseSemaphore` on `0xD` three times.
Workers 2–5 exit their waits and then re-enter them through normal title code.
No semaphore operation is synthetic. Evidence: `logs/cache_unavailable_fallback.stdout.log`
and the subsequent content runs.
