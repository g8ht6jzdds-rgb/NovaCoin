# NovaCoin protocol specification (draft v0)

**Status:** design baseline for the educational implementation.  This document
defines the consensus boundary before any consensus code is written.  A rule is
not active unless it is specified here and represented by a field in the
network parameters supplied to validation.  The word **MUST** is normative.

This is not a production-readiness claim.  In particular, neither a public
testnet nor mainnet is enabled by this document.

## 1. Scope and design rules

NovaCoin is a Bitcoin-inspired UTXO Layer-1 chain.  Consensus is limited to
byte parsing, transactions, scripts, blocks, proof of work, chain selection,
and the resulting UTXO state.  Peer transport, RPC, wallets, mining
coordination, indexes, and persistence are not consensus authorities.

### Non-consensus explorer boundary

A block explorer is a derived, read-only view of blocks that a node has already
accepted. Explorer indexes, address labels, fee displays, estimates, and query
results MUST NOT participate in block validation, chain selection, or UTXO
mutation. Any explorer feed MUST be one-way from copied node data into the
explorer; the explorer MUST NOT invoke state-transition APIs.

The same pure validation functions MUST validate a block from a peer, a block
read from disk, and a locally assembled candidate.  Validation receives only
the parsed object, the parent chain/UTXO context, and immutable
`NetworkParams`; it does not read wall-clock time, files, configuration, or
network state directly.  The caller supplies an explicit validation time where
a time rule needs one.

All monetary values, heights, counts, target values, and serialized integer
fields use bounded integer arithmetic.  Floating-point arithmetic is forbidden
in consensus paths.  An overflow, underflow, out-of-range conversion, or
ambiguous encoding is a consensus failure.

## 2. Primitive notation and canonical wire format

Unless a field says otherwise, integers are unsigned little-endian of the
stated width.  `i32` is two's-complement little-endian and `u256` is a
256-bit unsigned integer.  `hash256(x)` is `SHA256(SHA256(x))`, represented in
serialized fields as the 32 digest bytes in the order returned by SHA-256.
Human-facing hexadecimal displays reverse those bytes, as in Bitcoin; display
order is never used for hashing or comparison.

`CompactSize` is canonically encoded as follows:

| Value range | Encoding |
| --- | --- |
| `0..252` | one byte |
| `253..65535` | `0xfd` followed by `u16` |
| `65536..4294967295` | `0xfe` followed by `u32` |
| `4294967296..18446744073709551615` | `0xff` followed by `u64` |

The shortest permitted form MUST be used.  A decoder MUST reject a longer form
for a value that has a shorter form, truncation, and a count above the
applicable consensus limit before allocating or iterating.  Byte vectors are
`CompactSize(length) || bytes`, subject to their field limit.  Arrays are
`CompactSize(count) || elements`.

No consensus object permits trailing bytes when decoded from an independently
framed byte string.  All hashing preimages use the exact canonical
serialization below.  Alternative encodings, including non-minimal
`CompactSize`, are invalid rather than normalized.

## 3. Cryptography and identifiers

* Transaction ID (`txid`) is `hash256(canonical_transaction_serialization)`.
* Witness ID (`wtxid`) is not defined in v0 because v0 has no witness data.
* Block ID is `hash256(canonical_block_header_serialization)`.
* The transaction merkle root is defined in section 6.
* The v0 signature scheme is ECDSA over secp256k1 with SHA-256 message hashes.
  Implementations MUST use a maintained, reviewed crypto library; they MUST
  NOT implement SHA-256, secp256k1, or ECDSA themselves.

For all `u256` numerical comparisons (proof of work and targets), the byte
string is interpreted as a little-endian unsigned integer.  No signed
interpretation is permitted.

## 4. Consensus parameter model

Every validation entry point MUST receive one immutable `NetworkParams`.  It
contains every network-dependent consensus constant and no caller may replace
an individual constant ad hoc.

```text
NetworkParams {
  NetworkId id;                         // REGTEST, TESTNET, or MAINNET
  bool enabled;                         // admission gate, immutable at run time
  int32_t protocol_version;             // P2P version we advertise
  int32_t minimum_peer_protocol_version; // minimum permitted P2P peer version
  Hash256 genesis_block_id;
  BlockHeader genesis_header;            // exact header commitment
  Uint512 genesis_chainwork;

  Amount money_supply_cap;
  Amount initial_block_subsidy;
  uint32_t subsidy_halving_interval;
  uint32_t coinbase_maturity;

  uint32_t target_spacing_seconds;
  uint32_t retarget_interval;
  uint32_t target_timespan_seconds;
  Uint256 pow_limit_target;
  uint32_t pow_limit_compact;
  bool allow_min_difficulty_blocks;
  bool no_retargeting;

  uint32_t median_time_past_window;
  uint32_t max_future_block_time_seconds;
  uint32_t max_block_weight;
  uint32_t max_block_serialized_size;
  uint32_t max_transactions_per_block;
  uint32_t max_tx_serialized_size;
  uint32_t max_tx_inputs;
  uint32_t max_tx_outputs;
  uint32_t max_script_size;
  uint32_t max_script_element_size;
  uint32_t max_script_ops;
  uint32_t max_stack_items;
  uint32_t max_coinbase_script_size;
  uint32_t locktime_threshold;
  uint32_t max_sequence;
  uint32_t sig_hash_all;
}
```

`Uint256` and `Uint512` are fixed-width unsigned integer types with checked
operations; `Uint512` is used for accumulated chain work.  A chain-work
addition that would overflow `Uint512` is invalid.  `Amount` is `int64_t` and denotes atomic units only.  Valid amounts are
non-negative and at most `money_supply_cap`; a sum is computed only after
checking that its next addition cannot exceed `money_supply_cap`.  One
NovaCoin (`NOVA`) is exactly `100,000,000` atomic units, but no consensus
calculation uses a decimal conversion.

The parameter schema is normative.  The following *activation values* are
intentionally not finalized in this first document: all genesis commitments,
all subsidy values, the supply cap, every TESTNET and MAINNET PoW limit, and
every resource limit. The development-only REGTEST PoW limit is documented in
`docs/pow.md`; it is not an enablement decision for any network. Values MUST be
selected in a reviewable parameter decision, recorded in this document and in
compiled-in parameter tables, then covered by exact-vector tests before the
corresponding network is enabled. A missing or invalid parameter is a
startup/configuration error, never a default.

Initial enablement policy:

| Network | `enabled` | Permitted use |
| --- | --- | --- |
| `REGTEST` | `true` once its complete parameter table and genesis vector exist | local deterministic development |
| `TESTNET` | `false` | no public network until `docs/testnet-genesis-review.md` is fully approved and a separate immutable-table enablement change lands |
| `MAINNET` | `false` | must remain disabled until an explicit later decision |

An enabled network's genesis block is valid only if its canonical header hashes
to `genesis_block_id`, its parent hash is all zero bytes, its transaction list
and UTXO effects validate under that network's complete parameters, and its
full canonical block serialization matches the committed genesis fixture.

## 5. Transactions and UTXO state

An `OutPoint` is exactly `Hash256 txid || u32 output_index`.  It identifies a
previous transaction output.  A `Coin` in the UTXO view contains the referenced
`TxOut`, the creating block height, and whether it came from a coinbase
transaction.

```text
TxIn  = OutPoint previous_output
      || bytes<max_script_size> script_sig
      || u32 sequence

TxOut = i64 value
      || bytes<max_script_size> script_pubkey

Transaction = i32 version
           || array<max_tx_inputs> inputs
           || array<max_tx_outputs> outputs
           || u32 lock_time
```

An ordinary transaction MUST have at least one input and one output.  It MUST
be at most `max_tx_serialized_size`; each count and byte vector MUST obey its
network limit.  Every output amount MUST be valid.  Its output total MUST not
overflow or exceed `money_supply_cap`.

A coinbase transaction is recognized solely by having exactly one input whose
outpoint has all-zero `txid` and `output_index == 0xffffffff`.  It MUST have at
least one output, may occur only at transaction index zero of a block, and its
`script_sig` length MUST be in `[2, max_coinbase_script_size]`.  Its sequence
and lock time are serialized but have no special implicit values.  The v0
coinbase script MUST begin with the minimally encoded current block height;
the remaining bytes are opaque subject to the size limit.  A transaction that
uses the null outpoint but fails any coinbase rule is invalid.

The height prefix is a canonical ScriptNum direct push. Height zero is exactly
`0x00` (`OP_0`). For a nonzero height, encode its unsigned magnitude in the
shortest little-endian byte sequence, append `0x00` only when the final byte's
high bit would otherwise be set, then prefix those bytes with their direct push
length (`0x01..0x05`). The coinbase script may contain opaque bytes only after
this exact prefix.

For a non-coinbase transaction:

1. Every referenced outpoint MUST exist in the parent UTXO view.
2. No outpoint may appear twice in the transaction or elsewhere in the block.
3. A coinbase coin MUST have `spend_height - coin.height >= coinbase_maturity`.
4. `sum(inputs)` and `sum(outputs)` MUST be valid, checked sums; inputs MUST
   be at least outputs.  The fee is their exact non-negative difference.
5. Each input's unlocking script MUST satisfy the referenced locking script
   under section 7.
6. The transaction MUST be final under section 8.

After all transactions validate, the block atomically removes all spent coins
and adds all new outputs.  No partial state update is observable after a
failed transaction or block.

## 6. Blocks and merkle commitment

```text
BlockHeader = i32 version
            || Hash256 previous_block_id
            || Hash256 merkle_root
            || u32 time
            || u32 bits
            || u32 nonce

Block = BlockHeader header
      || array<max_transactions_per_block> transactions
```

The header is exactly 80 bytes.  A non-genesis block MUST have a parent known
to the candidate chain.  The block MUST include between one and
`max_transactions_per_block` transactions, begin with exactly one coinbase,
and contain no other coinbase transaction.  Its full serialized size MUST not
exceed `max_block_serialized_size`.  Until a witness design is explicitly
specified, `max_block_weight` equals the full serialized size and is checked
against the same serialized byte sequence.

To calculate the merkle root, start with each transaction's `txid` in block
order.  If a level has an odd number of hashes, duplicate its final hash.
Replace each adjacent pair `left, right` with `hash256(left || right)` until one
hash remains.  That hash MUST equal `header.merkle_root`.  A block with only
one transaction commits that transaction's `txid`.  The transaction ordering
and count in the block are therefore committed.  Duplicate transaction IDs
within a block are invalid, even if their serializations somehow differ.

The coinbase output total MUST be no greater than
`block_subsidy(height) + fees`. `block_subsidy` is the initial subsidy
right-shifted by `floor(height / subsidy_halving_interval)`; the final nonzero
era pays one atomic unit and later eras pay zero. All shift and addition
preconditions MUST be checked. This formula becomes active only when the
associated parameters are finalized.

## 7. v0 script profile

v0 supports a deliberately small, non-Turing-complete script profile:

* Locking scripts: `OP_DUP OP_HASH160 <20-byte pubkey-hash> OP_EQUALVERIFY
  OP_CHECKSIG` (P2PKH) only.
* Unlocking scripts have exactly two direct pushes:
  `push(len(der_signature) + 1) || der_signature || 0x01 || 0x21 || compressed_pubkey`.
  The first push opcode MUST be in `0x09..0x49`, the signature MUST be strict
  DER, and the sole sighash byte is `SIGHASH_ALL (0x01)`. The second push is
  always the direct 33-byte opcode `0x21`; no `OP_PUSHDATA*`, extra push, or
  alternate encoding is valid.
* The supplied public key's `HASH160(pubkey)` MUST equal the locking script's
  20-byte value.  `HASH160(x)` is `RIPEMD160(SHA256(x))`.
* Public keys MUST be valid compressed secp256k1 points.  Signatures MUST be
  strict DER, have low-S normalization, and verify against the computed
  signature hash.

No opcode outside this exact profile is valid.  Scripts, pushed elements,
opcode count, and stack item count MUST satisfy their respective parameters;
the exact-template requirement also eliminates alternate pushes and execution
ambiguity.

For `SIGHASH_ALL`, the signature preimage is the canonical serialization of a
copy of the spending transaction in which every input `script_sig` is empty
except the input being checked, whose `script_sig` is the referenced
`script_pubkey`, followed by `u32(0x00000001)`.  Its digest is `hash256` of
that preimage.  The DER signature's final sighash byte is excluded from DER
parsing and included only as the required `0x01` selector.  The script engine
must not accept any other selector.

## 8. Lock time and sequence

A transaction with `lock_time == 0` is final.  Otherwise, it is final if every
input sequence is `max_sequence`, or if `lock_time` is strictly less than the
comparison value.  Values below `locktime_threshold` compare to the candidate
block height; values at or above it compare to the candidate block's
median-time-past.  v0 defines no relative sequence locks; all non-final
sequence values only opt into this absolute-lock-time rule.

## 9. Proof of work, time, and difficulty

`bits` encodes a target using Bitcoin's compact-target format: an unsigned
8-bit exponent in the most significant byte and an unsigned 23-bit mantissa
in the remaining bits.  Decode it with exact integer operations.  A target is
invalid if its mantissa is zero, its sign bit is set, its decoded value cannot
fit in 256 bits, its encoding is non-canonical, or it exceeds
`pow_limit_target`.  Re-encoding a valid target MUST produce the original
`bits` value.

The header PoW is valid when the little-endian `u256` value of its block ID is
less than or equal to the decoded target.  This comparison is unsigned and
constant definition/encoding must not depend on host endianness.

For an enabled network, expected `bits` is deterministic and MUST be derived
inside consensus from immutable network parameters and the validated ancestor
header index. A peer, miner, RPC caller, or other adapter MUST NOT supply an
expected target to block acceptance.

* Genesis uses the exact `genesis_header.bits`.
* If `no_retargeting` is true, every descendant uses `pow_limit_compact`.
* Otherwise, at heights not divisible by `retarget_interval`, use the parent's
  `bits`, except where `allow_min_difficulty_blocks` is true: if the new block
  time is more than twice `target_spacing_seconds` after its parent, use
  `pow_limit_compact`.  For a timely block in that mode, start at its parent
  and walk backward while the examined block is not at a retarget boundary,
  has `bits == pow_limit_compact`, and was itself more than twice
  `target_spacing_seconds` later than its parent.  Use the first block's
  `bits` that does not meet all three conditions.  This precisely identifies
  only the preceding minimum-difficulty exceptions; an ordinary block whose
  target happens to equal the PoW limit is retained.
* At each retarget boundary, measure elapsed time from the first block in the
  preceding interval through the parent.  Clamp it to
  `[target_timespan_seconds / 4, target_timespan_seconds * 4]`.  Compute
  `new_target = min(pow_limit_target, old_target * actual_timespan /
  target_timespan_seconds)` with an exact wide integer product and truncation
  toward zero.  Canonically compact-encode it for expected `bits`.

Parameter validation MUST ensure `retarget_interval > 0`,
`target_timespan_seconds > 0`, `target_spacing_seconds > 0`, and multiplication
and interval arithmetic are safe before a network can be enabled.

For any non-genesis block, `header.time` MUST be strictly greater than the
median of up to `median_time_past_window` previous headers' times, and MUST be
no more than `validation_time + max_future_block_time_seconds`.  The median is
the middle element of the sorted integer times (for an even count, the lower
middle), computed without floating point.

`validation_time` is sampled only from a node's trusted local clock before
validation begins. It is never derived from, increased by, or otherwise
influenced by the candidate header. A rejected header MUST NOT alter local time
tracking. Any local `header.time + 1` bookkeeping uses checked arithmetic and
fails closed on overflow.

## 10. Chain selection and validation order

Chain work is accumulated as an exact unsigned value using
`work(target) = floor((2^256 - 1) / (target + 1)) + 1`.  The chosen valid chain
is the one with greatest cumulative chain work.  Equal-work ties are broken by
the lexicographically smaller raw 32-byte block ID, which makes selection
deterministic.  A candidate may be selected only after all headers and all
blocks from its fork point pass their applicable validation.

Block validation is performed in this order, failing closed at the first
failure:

1. Enforce framing, canonical decoding, count/size bounds, and no trailing
   bytes.
2. Validate header linkage, expected version (when a version policy is added),
   time, canonical target, expected difficulty, and PoW.
3. Validate the transaction count, one-and-only-first coinbase rule, transaction
   IDs, and merkle root.
4. Validate non-coinbase transactions in order against a temporary UTXO view:
   finality, duplicate spends, UTXO existence/maturity, amounts/fees, and
   scripts.
5. Validate the coinbase reward ceiling and atomically commit the temporary view.
6. Add exact block work and evaluate deterministic chain selection.

An implementation keeps a `BlockIndex` for every known valid block, including
the block ID, parent ID, height, decoded target, per-block work, cumulative
`Uint512` work, and status.  `Chain` stores the selected active tip and height;
`ChainState` owns the index and active UTXO state.  To activate a better
branch, it MUST build the fork paths, disconnect the old branch with its
stored undo data, connect the winning branch through the same `ConnectBlock`
pipeline, and only then publish the active tip.  On any failure it MUST restore
the original branch and UTXO state.  The detailed implementation contract is
in `docs/chain-selection.md`.

## 10.1 Durable block acceptance and observer notifications

Block persistence is a write-ahead journal, not a cache. A node durably flushes
a `prepare(block)` record before publishing the matching chain-state transition,
then durably flushes a `commit(block_hash)` record only after validation
succeeds. Startup replays only prepared records with matching later commits. A
truncated final record is an incomplete write and is ignored; a malformed
complete record or a commit without its preparation is fatal. If commit
persistence fails after acceptance, the node MUST roll the accepted block back
before returning an error.

Wallet and mempool observers receive active-chain transitions only. A valid but
inactive side-branch block MUST NOT be presented as confirmed or remove a
transaction from the active mempool. Reorganizations notify disconnects before
connects so observers can restore their local state.

## 11. Hostile-input limits and failure model

Network parsers MUST receive a caller-supplied frame no larger than
`max_block_serialized_size` for a block and `max_tx_serialized_size` for a
transaction.  They MUST enforce declared lengths before reading, allocating,
recursing, or multiplying sizes; reject zero/oversized/ambiguous counts where
the object requires a positive count; and use checked arithmetic for every
offset and aggregate.  Consensus code returns structured errors and MUST NOT
throw across a network boundary, abort, rely on assertions, or retain attacker
controlled byte views after their owner is released.

Validation errors are deterministic and category-stable (for example:
`noncanonical_compact_size`, `oversized_script`, `duplicate_input`,
`missing_utxo`, `bad_merkle_root`, `bad_pow`, `bad_coinbase_reward`).  Error
text itself is non-consensus diagnostic data.

## 12. Mempool policy is not consensus

The mempool is a node-local cache of transactions that appear valid against a
particular active UTXO set and next-block context.  Its admission fees,
capacity, retention, eviction, and template ordering are not consensus rules.
They MUST NOT change `CheckBlock`, `ConnectBlock`, `DisconnectBlock`, or chain
selection.  A block is valid only through the validation order in section 10,
whether or not its transactions were present in any mempool.  The v0 policy,
including dependency handling and mandatory revalidation after chain-state
changes, is documented in `docs/mempool.md`.

## 13. P2P transport is not consensus

Network framing, peer handshakes, address relay, inventory relay, connection
limits, and peer timeouts are node-local transport policy.  A `tx` or `block`
received from a peer has no consensus effect merely by parsing successfully.
It MUST flow through canonical deserialization, structural validation, the
ordinary consensus validation pipeline, and then chain-state processing.  The
transport layer MUST NOT mutate UTXOs, a block index, or the active chain.  The
v0 wire format and hostile-input controls are documented in `docs/p2p.md`.

## 14. Synchronization

A node starts synchronization only after P2P version handshake completion. It
requests headers from its verified tip, validates parent linkage and proof of
work before accumulating header chainwork, requests missing blocks, and sends
each received block through the normal `ChainState::AcceptBlock` pipeline. A
peer's advertised height, header claims, and block claims are never authority.
Only verified proof of work and fully validated blocks can affect chain
selection. The current deferred-difficulty synchronization contract is in
`docs/synchronization.md`.

## 15. Reference wallet boundary

The reference wallet is not consensus code.  It may generate keys, track
wallet-relevant outputs, construct and sign transactions, perform local
defense-in-depth signature checks, and submit transactions through an
application-defined broadcast boundary.  It MUST NOT mutate UTXO or chain
state, decide whether a transaction or block is valid, or cause an unvalidated
network message to affect consensus state.  Its P2PKH convention, deterministic
coin selection, fee calculation, and explicit non-features are documented in
`docs/wallet.md`.

## 16. Local RPC boundary

JSON-RPC is a localhost application interface, not a consensus channel.  It
MUST require configured authentication, reject non-loopback binding, bound and
validate hostile request data, and route transaction/block work only through
the ordinary wallet, mempool, mining, and chain-state interfaces.  It MUST NOT
export private keys.  The v0 methods and error contract are in `docs/rpc.md`.

## 17. Network parameter tables and genesis

The compiled-in REGTEST, TESTNET, and disabled MAINNET tables bind monetary,
PoW, difficulty, network magic, ports, address presentation prefixes, and a
genesis commitment.  Each genesis fixture has an all-zero parent, canonical
coinbase transaction and Merkle root, canonical target, valid PoW, and exact
hash test vector.  MAINNET remains **NOT FINAL — DO NOT DEPLOY**.  The
parameter values and generator input contract are in `docs/networks.md`.

## 18. Regtest integration harness

The REGTEST integration harness runs isolated local node runtimes with
deterministic time, data directories, block journals, wallets, ports, logs,
and explicit peer-link topology.  It replays persisted blocks through the
ordinary chain-state pipeline and uses the same block/transaction admission
routes as local relay.  It is a test facility, never a consensus authority;
the v0 scope and limitations are in `docs/regtest-harness.md`.

## 19. Required implementation evidence

Before enabling a network, its implementation MUST include deterministic unit
tests for every rule above, with at least one accepted and one rejected vector
per rule.  Required negative vectors include malformed/truncated and
non-minimal CompactSize values; all count/size limits; integer overflows;
invalid amounts; noncanonical targets; wrong PoW; wrong expected difficulty;
bad timestamps; broken merkle roots; duplicate txids/spends; coinbase placement
and reward errors; immature/missing UTXOs; lock-time failures; malformed or
high-S signatures; wrong P2PKH key hash; and chain-work tie handling.

Parser and consensus-deserializer fuzz targets MUST be deterministic with a
bounded input size and must assert no crash, unbounded allocation, undefined
behavior, or acceptance of malformed/noncanonical encodings.  Exact genesis,
transaction, header, target, signature-hash, and retarget test vectors are
required.  Tests never bypass or weaken validation.

## 20. Deliberately deferred decisions

The items below are not implicit defaults.  They require a reviewed update to
this specification and concrete `NetworkParams` values before implementation
can activate them:

1. Every genesis block fixture and identity commitment.
2. Resource, monetary, and PoW parameter values for REGTEST and TESTNET.
3. TESTNET activation date and public seed/transport policy.
4. A MAINNET parameter table, activation governance, and explicit enablement.
5. Any transaction-version upgrade, script feature, witness design, or soft/hard
   fork activation mechanism.

Until these decisions are made, implementations MUST expose no default public
network and MUST refuse to start TESTNET or MAINNET.
