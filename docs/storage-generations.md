# Local generation-store format and qualification

This is local storage metadata, not consensus serialization. REGTEST and TESTNET
daemon writers are routed through the durable node coordinator described below;
the old `RegtestNode` persistence path is isolated to test support. Historical
journal/wallet atomicity failures remain in the external evidence record. The
integration and component tests do not establish Gate 6, security, release, or
MAINNET readiness.

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

Successful candidate-image and metadata writes require an explicit successful
file close after their checked file flush. A close error prevents preparation
from becoming a publishable token. Close is not retried because a failed close
can already have released the descriptor. Destructor cleanup remains best-effort
on paths that have already failed. Synthetic close-result tests cover the five
candidate file types and repeated old-state recovery/retry; they do not claim an
actual device error, complete syscall-error coverage or power-loss qualification.

A separate per-store candidate-I/O model exercises the write loop with a short
successful write, zero progress, a simulated no-space error, a real one-byte
prefix followed by simulated no-space, and a failed file-sync result. It can
target journal, wallet, mempool, descriptor or root candidates. Tests assert
whether the modeled operation was actually reached: an empty image performs no
data write but still requires sync. These controls are in-process test APIs,
not node configuration or RPC. They do not exhaust the host disk, manufacture
actual device errors or replace platform durability qualification.

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

The parent-owned `nova.generation_crash` fixture launches only its own test child.
Its matrix includes before/after checkpoints and partial candidate-image writes;
fresh processes verify old or new authority and retention of previous images.
These controlled exits are distinct from interruption inside an OS syscall,
kernel failure or power-loss durability. The Windows native root-lease test
separately forces an actual replacement denial with an open handle, verifies
exact old-root/image recovery twice and then commits a new preparation. It does
not establish disk-full, flush-failure or power-loss coverage.

The experimental `DurableNode` owns synchronous publication and explicit
shutdown. Shutdown checks committed authority, prevents subsequent node/peer
writers and closes its transport. It does not save a separate wallet or mempool:
successful mutations must already belong to a committed generation. Unavailable
authority produces a failed shutdown result and requires recovery. A clean
shutdown result is subject to the same platform durability assumptions above.

`DecodeGenerationMetadata` is the same pure fixed-size decoder used by store
observation and staged-image checks. `fuzz_generation_metadata` exercises it
without file access under ASan/UBSan, with public identity-bound seeds under
`fuzz/corpus/generation_metadata/`. It cannot prove image durability, encryption,
consensus validity, allocation behavior of other parsers or node-level atomicity.

The bounded local acceptance run now covers the integrated daemon writer paths,
modeled journal/wallet/mempool/descriptor/root write and sync failures, selector
replacement denial, controlled process exits, repeated restart checks, and
1/10/100-block reorganization fixtures. The exact test names, commands, source
snapshot, binaries, and results are bound in the external Gate 6 evidence packet.
These checks do not inject failures inside operating-system syscalls, demonstrate
power-loss persistence, qualify a filesystem/device/controller/cache/barrier
combination, or provide independent review. Concurrent-publication guarantees
are limited to the coordinator serialization exercised by the recorded tests;
they are not a cross-process or hardware-durability claim. Development-only
legacy node APIs remain outside production daemon linkage. Gate 6 remains FAIL
until platform qualification and independent review are supplied and all other
acceptance evidence is verified. MAINNET remains locked.

## Daemon coordinator cutover

Initial selector creation uses the same checked write, flush and close path as
later candidate selectors. Startup verifies the selected images and requires a
successful directory barrier before returning a recovered store. A failed
startup barrier refuses admission without rewriting the selector or falling back.
An interruption before the first selector replacement leaves temporary evidence
and startup refuses to infer an empty database from it. An interruption after
replacement may leave a complete empty authority that a later startup verifies;
the failed startup is not a claim that replacement never occurred. Test-only
startup hooks exercise these boundaries with controlled exits and modeled I/O
errors. They do not strengthen the Windows or hardware durability claims above.

Recovery also accepts an empty-by-default injected test observer. It reports only
phase and replay ordinal/count, never key material or mutable state. The daemon
does not supply it and no RPC/configuration option selects it. Controlled child
exits can interrupt first/middle/final block replay, wallet loading, pending-pool
reconstruction and final admission. Restart tests must preserve all committed
image bytes and recover the same complete public state. These are process-exit
tests, not evidence of interrupted physical writes or power-loss durability.

REGTEST and TESTNET daemon startup now select only `DurableRuntime`. Blocks,
transactions, mining, key generation, payments and faucet admission enter its
node-owned coordinator, including peer callbacks. The retired legacy flag refuses
before storage opens. Existing legacy images require verified offline migration
to a new destination; no in-place rewriting or unpersisted mempool reconstruction
is performed. TESTNET bootstrap and transport configuration retain their prior
configuration semantics; RPC remains loopback-only. This does not authorize
public service operation.

Process-local metrics observe completed publication and transport activity.
Unavailable or ambiguous durable outcomes are not counted as validation rejection.
They quarantine the owner and require recovery. Metrics are not persisted or
consensus inputs. The old `RegtestNode` implementation and its peer constructor
are linked only through `nova_legacy_node_test_support` in historical regression
executables. They are absent from the production `nova_node` library and daemon
link graph. Their direct mutable getters and independent persistence remain
unqualified test behavior, not durable-coordinator proof. The compatibility
declaration alone does not provide a production implementation. Gate 6 remains
FAIL pending the other requirements above; link isolation alone does not prove
full recovery acceptance.

On Windows, `nova.durable_root_denial` exercises a real selector DELETE-sharing
denial through daemon address generation, payment submission and mining. It
requires each rejection to preserve the selector and RPC-visible state, verifies
that state after restart, then verifies successful payment/mining retries across
two restarts. It uses disposable loopback fixtures and process termination, not
power interruption. It does not cover other write/flush failures or qualify
filesystem durability. Earlier depth-100 timeout/resource-abort records remain
preserved as historical failures. The current bounded depth-100 fixture passes
within its recorded budget, but this does not establish unbounded reorganization
capacity or production-scale performance.
