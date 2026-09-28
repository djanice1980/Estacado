# Startup synchronization

The Xbox critical-section shape used by the runtime is the verified 28-byte
Xenia `X_RTL_CRITICAL_SECTION` representation: dispatch header at offset 0,
lock count at `+0x10`, recursion count at `+0x14`, and owning guest-thread
object at `+0x18`. `RtlInitializeCriticalSection` writes that initial state;
enter/leave preserve recursive ownership in guest-visible fields.

Contention waits on a condition variable and wakes one eligible guest waiter
when recursion reaches zero. Ownership uses persistent guest object identity,
not a host thread ID. Regression coverage verifies recursive entry, contention,
wake on leave, and wrong-state rejection.

`NtWaitForSingleObjectEx` now shares the same typed object foundation. It
waits without busy spinning, consumes auto-reset event/semaphore signals,
supports zero/relative/absolute 100 ns timeouts, returns `STATUS_TIMEOUT
(0x102)` on expiry, and marks the calling guest thread `Waiting` while blocked.
In the 10-second bounded title probe, all four workers block correctly on
startup semaphore handle `0xD`, whose initial count is zero. No semaphore
release was observed, so none was invented.
