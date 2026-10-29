# Local generation-store format and qualification

This is local storage metadata, not consensus serialization. The generation
store is not wired into node admission or wallet/RPC persistence yet. The
reproduced journal/wallet atomicity failure remains **FAIL**. No Gate 6, security,
release, or MAINNET readiness claim follows from these component tests.

## Authority and bytes

`generation.current` is the only authority. No directory scan selects a newer
or older generation. It has exactly 85 bytes, without padding or trailing bytes:

| Offset | Width | Meaning |
|---|---|---|
| 0 | 8 | ASCII `NVROOT01` |
| 8 | 1 | existing `NetworkId` byte |
| 9 | 4 | existing network magic, unsigned little endian |
| 13 | 32 | existing genesis hash bytes, in `Hash256::bytes()` order |
| 45 | 8 | unsigned sequence, little endian |
| 53 | 32 | single SHA-256 of the exact descriptor bytes |

Sequence zero represents an empty store and requires an all-zero descriptor
hash. It is created only in a new private empty directory (apart from the lease
file). A missing root with retained generation files fails closed. A malformed
root, identity mismatch, unknown version, bad length/hash, missing member or
unavailable read never permits fallback to an older retained generation.

For sequence `s > 0`, names are `generation-` followed by exactly sixteen lower
case hexadecimal digits, most significant digit first, and `.descriptor`,
`.journal`, or `.wallet`. Names are generated internally, never read as paths
from metadata. The descriptor has exactly 133 bytes:

| Offset | Width | Meaning |
|---|---|---|
| 0 | 8 | ASCII `NVGEN001` |
| 8 | 37 | network identity bytes as above |
| 45 | 8 | sequence matching the selector, unsigned little endian |
| 53 | 8 | journal byte length, unsigned little endian |
| 61 | 32 | single SHA-256 of journal image |
| 93 | 8 | wallet byte length, unsigned little endian |
| 101 | 32 | single SHA-256 of wallet image |

The images are opaque to this primitive. The coordinator must validate/replay the
journal and supply an already encrypted, validated wallet image using the existing
wallet format. The store accepts no key or passphrase. Tests use non-secret fake
images and do not assert that arbitrary image bytes form a usable journal/wallet.
Hashes detect mismatches, not forgery by a privileged host owner.

## Preparation and commit

One store holds an exclusive `generation.lock` lease for its lifetime; neither
it nor prepared tokens are movable. Callers must serialize all store operations
and must not alter sources during preparation. The private directory must already
exist under trusted local custody. Store files and input images must be regular,
single-link files. Symlinks/reparse points in supplied paths are rejected; POSIX
member access is anchored with `openat`/`O_NOFOLLOW`, Windows opens final objects
with `FILE_FLAG_OPEN_REPARSE_POINT`. This is not a sandbox against privileged
ancestor-directory changes or a hostile administrator. ACL/account hardening
remains the operator's responsibility.

Preparation reserves a new sequence with exclusive creation, streams copies and
SHA-256 in 64 KiB buffers, flushes member files and writes the descriptor and new
selector privately. POSIX synchronizes the containing directory before root
replacement. Only root replacement commits authority. Before replacement, old
authority, staged image hashes and exact new selector bytes are verified again.
Old images and abandoned preparations are retained; no cleanup or repair runs
silently. At most 1,024 candidate slots are tried per preparation, without unsigned
wrap. Exhaustion is a local storage error, not a consensus limit or permission to
delete evidence. Hashing and complete-journal copying have O(history) I/O cost;
production capacity and retention management remain unqualified.

`Commit` returns `Committed`, `NotCommitted`, or `Unavailable`. Errors after
replacement or at the final barrier return `Unavailable`, never final rejection.
`Resolve` verifies exact old/new authority and member hashes and retries the
directory barrier. Another generation, an unreadable root, corrupt members or a
wrong-owner token remains unavailable. Tokens are one-attempt; subsequent callers
use `Resolve`, not a second blind commit. A preflight observation failure performs
no replacement and permits the original attempt to be tried when context recovers.

## Platform evidence and pending integration

POSIX uses checked file `fsync`, same-directory `renameat`, and directory `fsync`.
A file flush alone does not establish directory-entry persistence; see the
[Linux fsync contract](https://man7.org/linux/man-pages/man2/fsync.2.html).
Windows uses checked `FlushFileBuffers` on members and
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` for the selector, with no cross-volume
copy flag. The [Windows move contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
does not establish equivalence to POSIX directory `fsync`; the Windows branch has
no separate directory barrier. Physical power-loss durability on Windows remains
**UNVERIFIED**. Successful API returns and controlled error seams are not hardware
power-loss proof on either platform. Platform/filesystem qualification is required
before this can support a launch gate or a durable coordinator claim.

The parent-owned `nova.generation_crash` fixture launches only its own test child,
which exits abruptly without destructors after six controlled returned error
checkpoints. Two fresh processes verify the exact old or new image pair and
retention of the old images. This proves the tested process-restart paths, not
interruption inside an OS syscall, kernel failure or power-loss durability.

`DecodeGenerationMetadata` is the same pure fixed-size decoder used by store
observation and staged-image checks. `fuzz_generation_metadata` exercises it
without file access under ASan/UBSan, with public identity-bound seeds under
`fuzz/corpus/generation_metadata/`. It cannot prove image durability, encryption,
consensus validity, allocation behavior of other parsers or node-level atomicity.

Remaining work includes inside-syscall fault/crash coverage, actual syscall
failures and device/barrier evidence, migration retaining legacy files, prepared
chain/wallet/mempool integration under exclusive publication, all key-generation
and RPC writers, outcome adapters, restart consistency, concurrent-reader design,
and the complete 1/10/100-block reorganization and crash/restart matrix. No existing
node storage authority changes in this PR. MAINNET remains locked.
