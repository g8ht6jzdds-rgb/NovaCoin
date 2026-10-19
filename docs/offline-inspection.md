# Offline chain inspection — Linux first

`nova-chain-inspect` is a read-only reporting utility, not a node, wallet, migration
tool, custody attestation or launch approval. It never calls node startup, journal
Open/recovery or identity Ensure on inputs. It uses the compiled network initializer
and ordinary validators. TESTNET/MAINNET daemon gates remain disabled; MAINNET and
non-Linux command execution are unsupported. No actual legacy/live TESTNET inspection
is authorized by this implementation.

## Build provenance

Normal project CMake builds include the command on Linux. Git checkout HEAD commit/
tree references are detected; copied-source builds must provide 40-lowercase-hex
`NOVA_INSPECT_SOURCE_REVISION` and `NOVA_INSPECT_SOURCE_TREE`. UNRECORDED references
prevent a successful command report. Record actual source-blob/dependency/toolchain
and binary hashes separately: build strings are references, not independent proof
of build honesty, reproducibility or platform/release support.

## Capture and invocation

Require an authorized coherent stopped-node capture, externally captured expected
tip/height/work and as-of time, and retained capture evidence. This tool does not
stop/copy a real node or independently verify operator honesty. Immutable bytes
alone do not prove their original coherent capture. Preserve originals; inspect
only disposable immutable snapshots containing exactly `network.identity`,
`blocks.dat` and `snapshot.meta`. An explicitly empty journal is genesis-only;
missing journal is an error. No wallet/key/credential/log/cache files are inputs.

```
nova-chain-inspect --snapshot /absolute/readonly-snapshot /absolute/separate-output
nova-chain-inspect --sealed-fds ID_FD JOURNAL_FD METADATA_FD /absolute/separate-output
```

Directory mode requires read-only filesystems at the regular-file handles, no extra
hard links, exact directory inventory, no symlink/path traversal, and no source/
output ancestor aliases. chmod-only files are insufficient. Sealed mode requires
Linux descriptors >=3 with WRITE/GROW/SHRINK/SEAL seals, not secrets in arguments.
It also enables disposable tests without persistent mounts. Malicious admins,
alternative writable mount views or compromised kernels remain outside the trust
boundary; use sealed descriptors and trusted capture provenance when appropriate.
Input identities/bytes are rechecked; no input permission/ownership changes occur.

Metadata is strict LF-only text with final LF and exactly ten lines:

```
nova-snapshot-v1
network=regtest
as_of=1704160000
tip=<64 lowercase hex, raw digest byte order>
height=0
work=<128 lowercase hex chainwork bytes>
identity_sha256=<64 lowercase hex>
journal_sha256=<64 lowercase hex>
provenance_sha256=<64 lowercase hex of retained capture evidence>
capture=stopped-immutable-copy
```

Network is regtest/testnet. Decimal fields have no redundant leading zeros.
Replace bracketed fields with actual captured facts. As-of is external context,
never inferred from block headers; later-time replay does not prove historical
arrival-time acceptance. Provenance references remain subject to human review.

## Complete accounting and report

Only final active-chain outputs/fees count; every committed branch traverses normal
validators. Genesis is counted once via the initializer (frozen TESTNET allocation,
REGTEST initial zero), not as an ordinary WAL child. Enumerate every active output
key and initial key; authoritative GetCoin plus count equality provides a complete
set, not wallet/RPC sampling. Retain zero-value, immature, unsupported-script and
non-owned outputs. UTXO value is not proven economic spendability or known keys.

Checked fixed-width accounting uses U0 initial value, S scheduled subsidy at actual
active heights >=1, F validated fees, C actual coinbase payouts, D=S+F-C combined
underclaim. Require complete UTXO value `U0+sum(C-F) == U0+sum(S)-sum(D)`. Fees
recycle value, not issuance; C-F can be negative. Underclaim has no unique fee-burn/
unissued-subsidy split. Cumulative fees/payouts can exceed the circulating cap;
reporting overflow is incomplete, not a new consensus rule. Unsupported scripts
or lost keys are not silently removed as burns. Creator outpoint/spend and current
same-script balance are separate observations, not custody or coin-taint proof.

JSON has fixed field order, decimal strings, source/tree/binary/input hashes,
expected/observed tip/work/context, counts/accounting/creator/limits/limitations,
and complete sorted UTXO records. Record format:
`raw_txid:vout:value:height:coinbase:script_byte_length:script_hex`, sorted by raw
txid then numeric vout. Digest hashes these records with LF; it is non-consensus.
No volatile time/hostname enters canonical bytes. Identical inputs/context/tool
produce identical reports; capture evidence must still be reviewed independently.

Success exclusively creates a new output `report.json`, never overwriting an
existing report. A bounded anonymous Linux O_TMPFILE inode is fsynced before atomic
no-replace publication from its open descriptor through `/proc/self/fd`. No
replaceable temporary pathname can substitute different bytes. Unsupported output
filesystems fail closed; anonymous files disappear on failure/process death.
Only final report plus exit 0 is complete. Post-publication directory sync is best effort,
not a power-loss durability certification. No automatic upload/publication.

## Bounds and failure

Fixed initial local limits: 64 MiB journal, 100,000 records, 10,000 committed blocks,
10,000 pending prepares, 1,000,000 candidate keys, 256 MiB listing/report, 45-byte
identity, 2 KiB metadata. Decoder budgets precede payload allocations; aggregate
input, key and output limits complement compiled per-block limits. Command limits
are 120 CPU/wall seconds and 2 GiB native address space (64 TiB ASan virtual-mapping
allowance, NOT an RSS guarantee). Process/allocation death is non-success. Larger
chains need a reviewed resource profile; there are no overrides in this version.

Exit 0 / OFFLINE_SNAPSHOT_VALID applies only to these bytes, rules and declared
context, with separate completeness/replay/accounting/initialization flags. Every
required mismatch, corrupt/noncanonical/torn/uncommitted journal, wrong identity/
expectation, budget or I/O/output failure is nonzero and creates no new final
report. Stable stderr categories include journal_*, replay_*, resource_*; no valid
partial-prefix certificate or repair. Interrupted temporary outputs are ignored.

Tests use known synthetic keys only. Frozen public TESTNET commitment tests do not
use the real creator key. Actual-chain supply conformance, legacy compatibility,
independent review, custody, public operations, signed releases and activation
remain separate gates. Passing fixtures does not make the project production-ready.
