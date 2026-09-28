# Guest threading model

The startup guest thread and each `ExCreateThread` worker receive persistent
guest identities. The runtime currently maps each reached guest thread 1:1 to
a host `std::thread`; each gets a fresh PPC context, allocated guest stack,
inherited `r13`, thread-table object, TLS identity, priority state, affinity
state, FPU-exception mode, and execution state.

Observed startup creates four workers (IDs 2 through 5) through
`0x828ADA98 → 0x821FBB00`. They successfully resolve the current-thread
pseudo-handle and execute `KeEnableFpuExceptions(0)`. That call now stores a
per-guest-thread mode. It does not change the host floating-point exception
mask because translated helpers own host FP control state and host-wide changes
would be observable outside the caller's guest thread.

`KeSetAffinityThread` and `KeSetBasePriorityThread` update the referenced
guest thread object's stored affinity and base priority, returning the prior
value where the reached ABI requires it. No host scheduler enforcement is
claimed yet.

Guest states are `Running`, `Waiting`, `Blocked`, and `Terminated`. `ExTerminateThread`
and normal worker return mark a thread terminated; waits on a terminated
thread object may be signaled by the common object wait primitive.
