# Console signing blocker — verified 2026-08-24

## Dynamic call

The live title reaches `XeKeysConsolePrivateKeySign` on guest thread 1 after
the original worker group has started and thread 6 has been created.

| Field | Value |
| --- | --- |
| Import | `XeKeysConsolePrivateKeySign` |
| Guest return address | `0x828A8858` |
| Caller chain | `sub_828A92A8` → `sub_828A87A8` |
| Input (`r3`) | `0x6FFF EEC0`, SHA-1 digest buffer |
| Output (`r4`) | `0x6FFF F3A4`, console-signature field |
| `r5`–`r9` | zero / unused on this reached call |

Immediately beforehand the title invokes `XeCryptSha` with:

```
input  = r30 + 556
length = 88 bytes
output = 0x6FFF EEC0
length = 20 bytes
```

The SHA-1 is now implemented and its `abc` known-answer test passes. The
title then passes that digest to `XeKeysConsolePrivateKeySign`, later reads
and writes the raw virtual cache partition at offset `0x800`.

## Verified cache role and failure behavior

This is a raw-cache record, not a demonstrated DRM, save-game, profile, or
licensing operation. `sub_828A92A8` reads a 1024-byte record at raw offset
`0x800`, checks magic `0x4A6F7368`, and validates a signature at record `+4`.
Both signing and validation hash 88 bytes at record `+556`.

An all-zero read takes the normal cache-miss path: it initializes a replacement
record and calls `sub_828A87A8` to sign it before writing it back. The sign
call's return value is not consumed: immediately after return `r3` is
overwritten with the file handle for the subsequent read. There is no guest
branch for a signing failure, no status conversion, and no retry. Returning an
invented failure status would therefore not select a cache-disabled path; it
would merely let the title attempt to persist an unsigned record.

The sole `XeKeysConsoleSignatureVerification` call site is the corresponding
cache-record validation helper. A missing magic or failed verification routes
to the same record-rebuild path. See `docs/CACHE_SIGNING_ANALYSIS.md` for the
complete producer, consumer, and control-flow evidence.

## Why this is a blocker

`\Device\Harddisk0\partition0` is a raw cache/STFC path, not a game file.
The extracted game directory therefore cannot supply its persistent raw media
or an Xbox console private key. The public Xenia source has an explicit null
device for this raw path, but has no implementation of
`XeKeysConsolePrivateKeySign`; its documented crypto code implements SHA and
other public operations, not console-private signing.

The current call supplies no key parameter. Generating a host key, returning a
zero signature, or reporting success without a signature would manufacture
console identity and violate the observed signed-record flow.

## Safe completed work

- Object-pointer duplication creates independent live handles for one object.
- The raw cache partition is explicitly unavailable; no raw cache bytes are
  synthesized and no signed record can be written.
- `XeCryptSha` implements the verified three-input SHA-1 ABI.
- No raw game data was copied or modified.

## Resolved portable policy

The normal cache-unavailable path is now verified dynamically. Returning
`STATUS_OBJECT_NAME_NOT_FOUND` from the raw partition open causes the title to
convert the error, continue its environment fallback, create more workers, and
legitimately release startup semaphore `0xD`. The runtime therefore exposes no
raw cache medium, leaves the signing import unresolved, and never writes a
partial or fabricated signed record. This is not a graphics initialization
boundary.
