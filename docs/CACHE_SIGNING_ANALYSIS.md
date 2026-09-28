# Cache signing analysis

## Scope and classification

This analysis covers the reached raw-cache path only. It does not identify the
record as DRM, a saved game, profile data, or licensing data. The binary
evidence instead identifies a title-managed raw cache/STFC record.

| Item | Classification | Evidence |
| --- | --- | --- |
| Raw path | VERIFIED | `\\Device\\Harddisk0\\partition0` is opened by `sub_828A92A8` at `0x828A9384`. |
| Record I/O | VERIFIED | A 1024-byte transfer is read at byte offset `0x800` (`0x828A93C0`). |
| Cache-record magic | VERIFIED | Word zero is compared with `0x4A6F7368` at `0x828A93DC`. |
| Signature location | VERIFIED | Both sign and verify use record `+0x4` (`0x828A8858`, `0x828A8778`). |
| Signed bytes | VERIFIED | SHA-1 hashes exactly 88 bytes at record `+0x22C` (`0x828A884C`, `0x828A8768`). |
| Record payload copied for write | VERIFIED | The write helper copies 644 bytes of record state into the 1024-byte sector buffer before `NtWriteFile`. |
| Signature byte length | UNKNOWN | Neither reached API call supplies a length argument, and no legitimate implementation in the pinned/local references defines it. |

## Exact producer and consumer chain

`sub_828A92A8` reads the sector into a stack record buffer. On a magic mismatch
or failed validation it initializes a new record, then calls
`sub_828A87A8` at `0x828A94C4`.

`sub_828A87A8` performs this exact sequence:

```text
record + 0x22C, 88 bytes
  -> XeCryptSha
  -> 20-byte digest at stack + 0x90
  -> XeKeysConsolePrivateKeySign(digest, record + 0x4)
  -> copy 644-byte record into a 1024-byte sector buffer
  -> NtWriteFile(offset 0x800)
  -> NtClose
```

The signing import at `0x828A8858` receives only the digest and destination;
the next instruction replaces `r3` with a byte-count value. There is no
comparison, branch, retry, or status propagation from the call. Therefore no
verified failure status exists that a portable runtime can return to make the
guest select a cache-miss branch.

## Existing-record validation

The only `XeKeysConsoleSignatureVerification` call site is
`sub_828A8728` at `0x828A8778`. It hashes the same 88 bytes at `+0x22C` and
passes the digest plus signature at `+0x4` to the verification export. The
helper returns valid only when both the export result and its guest output word
indicate success. `sub_828A92A8` treats a failed helper result exactly as a
cache miss and goes to the new-record path at `0x828A9490`.

This gives the normal missing/invalid-cache behavior when a raw cache medium
exists:

```text
existing Partition0 read
  -> magic mismatch
  -> initialize replacement record
  -> console-private signature required
  -> write replacement sector
```

The portable runtime deliberately does not expose a raw Partition0 medium, so
it selects the earlier, verified `NtCreateFile` missing-device branch instead.

## Cache policy decision

`FscSetCacheElementCount(0, 32)` is reached before this path. The local Xenia
reference labels both values unknown and returns success; it provides no
documented zero-elements or cache-disabled behavior that the title reaches.
Changing the count would alter a title-selected configuration. In contrast,
returning the normal missing-device result for an absent raw partition exposes
the exact failure state the title already handles.

The raw-device-open failure path does propagate an error from `sub_828A92A8`,
but no dynamically reached caller has been shown to continue startup after
that error. It is not evidence for a safe portable no-cache policy. In
contrast, the dynamically selected normal cache-miss path explicitly rebuilds
the record and invokes console-private signing without a tested failure path.

Selected policy: **CACHE UNAVAILABLE / CACHE MISS**. The raw partition now
returns its real missing-device result. The unmodified title converts it to
`ERROR_FILE_NOT_FOUND (2)`, continues through its normal environment/config
fallback, creates additional workers, and releases startup semaphore `0xD`.
No console-private signing call, signature bytes, or persistent cache record
is produced.

This policy is deterministic across launches and does not modify game data.

## Consequence

The cache-signing blocker is resolved by the title's verified cache-unavailable
path. No graphics/Xenos API has been reached. The next runtime boundary is
ordinary filesystem/content startup, not console identity.
