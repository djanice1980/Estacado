# Runtime object manager

## Verified contract and title evidence

The runtime owns a typed table rather than treating a handle as a host pointer
or a guest object address. Every live entry has a synthetic integer handle, a
separate opaque guest-object address, type, handle count, reference count, and
closed state. Current types are `Thread`, `Event`, and `Semaphore` only.

`ObReferenceObjectByHandle` is reached at `0x828A7EB4`. The title passes the
thread handle returned by `ExCreateThread`, type token `0x1B000000`, and an
output pointer. It returns the table's guest-object address and increments the
reference count. The caller immediately uses that address for base-priority
and affinity operations, then reaches `ObDereferenceObject`. A wrong reached
type returns `STATUS_OBJECT_TYPE_MISMATCH (0xC0000024)`; an invalid or closed
handle returns `STATUS_INVALID_HANDLE (0xC0000008)`.

The worker bootstrap also references `0xFFFFFFFE`. Local Xenia source confirms
this is the current-thread pseudo-handle convention; the runtime resolves it
to the calling guest thread object and does not fabricate a handle.

Closing a handle and dereferencing an object are separate. `NtClose` removes
the externally usable handle; an object remains alive while a reference is
held. The opaque guest thread address deliberately contains no invented
KTHREAD layout: the title has only passed it back into verified kernel APIs.

## Object-pointer opens

`ObOpenObjectByPointer` is reached four times by thread 6 after the original
four worker threads are created. The live run opens existing thread objects
`0x7D0001A0`, `0x7D0001C0`, `0x7D0001E0`, and `0x7D000200`, creating distinct
handles `0x19` through `0x1C`; each is immediately accepted by
`ObReferenceObjectByHandle`. See `logs/crypt_sha_followup.stdout.log`.

The actual title wrapper is `sub_828A7E20`: it first calls
`ObLookupThreadByThreadId` with its `r5` thread ID, then passes the resulting
object pointer in `r3` and a stack output pointer in `r4` to the import at
`0x828A7E50`. It immediately calls `ObDereferenceObject` on the lookup result
and returns the newly written handle. No type token, desired access, access
mode, or handle-attribute argument is supplied by this reached two-argument
call, so none is silently modeled.

The implementation creates an independent handle-table entry that retains the
same object. Closing one duplicate removes only that handle; the object is
retired only after its last handle and last explicit reference are gone. An
unknown guest-object address returns `STATUS_UNSUCCESSFUL (0xC0000001)` and a
wrong requested type returns `STATUS_OBJECT_TYPE_MISMATCH (0xC0000024)`.
This matches the local Xenia pointer-open contract without treating guest
addresses as host pointers or inventing a KTHREAD layout. Regression coverage
verifies duplicate lifetime, close/reference ordering, stale addresses, and
type validation in `runtime_import_tests`.

## Classification

| Item | Classification |
| --- | --- |
| Handle lookup, reference count, close versus dereference | VERIFIED API contract / runtime implementation |
| Xenia's concrete object-table types and tokens | Reference implementation detail; not copied |
| `0x1B000000` thread-type token | The Darkness-specific requirement from the reached call |
| Undocumented guest KTHREAD fields | UNKNOWN; not fabricated |
