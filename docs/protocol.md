# NovaCoin Protocol Specification v0

**Status:** normative for the reviewed REGTEST and TESTNET fixtures. MAINNET is
**UNFINALIZED** and MUST NOT start. **MUST**, **MUST NOT**, **SHALL**, **SHALL
NOT**, **SHOULD**, and **MAY** are normative. A code, document, or vector
disagreement MUST be reported as `INCONSISTENT`; it MUST NOT be silently fixed.

## 1. Terminology

A node validates blocks; a peer is untrusted transport. A transaction spends
outpoints and creates outputs. An outpoint is `(txid, output index)`; a UTXO is
an unspent output with creation height and coinbase flag. A block contains a
header and ordered transactions. A chain is parent-linked valid blocks; the
active chain is selected by cumulative chainwork. A coinbase is transaction
zero with the null outpoint. Confirmation means active-chain inclusion plus its
active descendants. A target is a PoW upper bound; difficulty derives targets.

## 2. Primitive data types

`u8/u16/u32/u64` are unsigned little-endian; `i32/i64` are two's-complement
little-endian. `Hash256` is 32 raw bytes, and PoW treats it as unsigned
little-endian. Fixed arrays have no length prefix. Vectors are
`CompactSize(length)||bytes`; arrays are `CompactSize(count)||elements`.
CompactSize uses the shortest form: `0..252`; `fd||u16`; `fe||u32`; or
`ff||u64`. Decoders MUST reject truncation, non-minimal lengths, over-limit
counts and arithmetic overflow. Count, length, overflow and remaining-byte
checks MUST precede the corresponding allocation. Bounded temporary parsing is
permitted; trailing bytes MUST reject before successful whole-message admission.
This ordering does not change the set of accepted serialized inputs.

## 3. Cryptographic primitives

`hash256(x)=SHA256(SHA256(x))`; `HASH160(x)=RIPEMD160(SHA256(x))`. Txids hash
canonical transaction bytes and block IDs hash canonical 80-byte headers.
v0 uses compressed secp256k1 public keys and strict-DER low-S ECDSA signatures
over Section 8 digests. A public key MUST be exactly 33 bytes, start with `02`
or `03`, represent a point on secp256k1, and reserialize identically in compressed
form. The curve is `y^2=x^3+7 mod p`, with
`p=0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f`.
The prefix selects the even (`02`) or odd (`03`) y coordinate; the remaining
32 bytes encode x as an unsigned big-endian integer less than p.
The subgroup order is
`n=0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141`.
A signature MUST be 8..72 bytes of DER: `30 L 02 LR R 02 LS S`, with
one-byte lengths, `L=4+LR+LS`, and no remaining bytes. R and S are positive,
minimal big-endian signed INTEGER encodings: no negative high bit and no leading
zero except a necessary sign pad. Their integer values MUST satisfy
`1 <= R < n` and `1 <= S <= floor(n/2)`. Parsing and DER reserialization MUST
be byte-identical. Format acceptance alone does not establish ECDSA validity. Implementations MUST use maintained libraries and MUST
NOT introduce a novel primitive. Raw digest byte order is used in all vectors.

## 4. Address format (non-consensus)

P2PKH presentation addresses are Base58Check of `prefix || HASH160(pubkey)`;
the checksum is the first four raw bytes of `hash256(payload)`. Invalid
characters, empty text, wrong decoded length/prefix, or bad checksum MUST
reject. Addresses do not determine block or transaction validity. REGTEST and
TESTNET prefixes are 111 and 112; MAINNET is **UNFINALIZED**.

## 5. Transactions

```text
Transaction=i32 version||array<inputs>||array<outputs>||u32 lock_time
TxIn=Hash256 previous_txid||u32 previous_index||bytes script_sig||u32 sequence
TxOut=i64 amount||bytes script_pubkey
```

Txid is `hash256` of exactly these bytes. A transaction MUST have 1..128 inputs
and outputs, be at most 100,000 bytes, use scripts at most 256 bytes, and have
unique input outpoints. Amounts are checked in `0..MAX_MONEY`; output sums MUST
NOT overflow or exceed `MAX_MONEY`. Version MUST NOT select an alternate v0
rule. `transactions.ncpv` is a serialized txid vector.

## 6. Transaction inputs

A non-coinbase input is valid only if the referenced outpoint exists in the
temporary UTXO view, is not already spent in the transaction/block, is mature
when coinbase, is final, and satisfies Section 8. `sequence` participates in
serialization and absolute lock time only; v0 has no relative locks.

A null outpoint is exactly a zero 32-byte txid AND index `0xffffffff`.
A zero txid with any other index is malformed. A nonzero txid with index
`0xffffffff` is not malformed by shape alone; existence and spendability MUST
still be checked. Exactly one null input identifies coinbase shape; placement
and reward checks remain separate.

After structural validation, a transaction is final iff `lock_time == 0`, or
every input sequence equals `0xffffffff`, or its lock time is strictly below
the comparison value. That value is the candidate height when
`lock_time < 500000000`, otherwise median time past. Equality fails that last
comparison; the zero-locktime and all-final-sequence alternatives still apply.
These are existing validation bindings, not new relative-lock rules.

## 7. Transaction outputs

An output is an `i64` amount and bounded locking bytes. Zero is permitted;
maximum is `MAX_MONEY`. Unknown structurally valid scripts MAY enter the UTXO
set but are not spend-authorized. v0 has no general script interpreter or
implicit burn rule.

## 8. Signature authorization

The sole spend-authorized locking form is
`76 a9 14 <20-byte HASH160(pubkey)> 88 ac`. The unlocker is exactly two direct
pushes: strict-DER low-S signature plus selector `01`, then a 33-byte compressed
key. The signature push is 9..73 bytes including selector `01`; the key push
is exactly 33 bytes. Alternate push forms, selectors, extra bytes, invalid points, high-S, and
key-hash mismatch are invalid. For `SIGHASH_ALL=1`, clear every input script,
replace the checked input script with the referenced locking bytes, canonically
serialize, append `u32le(1)`, then hash256. `sighash.ncpv` binds a preimage and
digest. `signature_auth_*.ncpv` adds scoped full-authorization examples, including
unchanged-signature/different-digest rejection and push-boundary rejection; the
record inventory in `test/data/protocol/FORMAT.md` identifies each case. These
examples do not expand the spend-authorized form or select another sighash mode.

## 9. UTXO model

Apply regular transactions in order to parent UTXOs plus earlier regular
outputs, minus earlier spends. Forward references and duplicate spends reject.
Coinbase outputs are added only after regular validation, so same-block
coinbase spends reject. Checked `sum(inputs)>=sum(outputs)` is required and
`fee=sum(inputs)-sum(outputs)`. Missing inputs, underflow/overflow, or a parent
or staged outpoint collision reject. The complete transition is atomic.

## 10. Coinbase transaction

Only transaction zero MAY be coinbase. It has one null outpoint, at least one
output, and a 2..100 byte script. An ordinary coinbase script MUST begin with
the minimally encoded direct-push current height (`00` at height zero).
For a nonzero u32 height, encode its magnitude little-endian without redundant
zero bytes, append a zero sign pad iff the last byte has bit 7 set, and prefix
the resulting length as one direct-push byte. Only this prefix encodes height;
remaining bytes MAY follow within the existing 2..100-byte total bound.
Height zero therefore still needs at least one suffix byte. The
committed REGTEST/TESTNET genesis serializations are explicit anchor exceptions
and use their documented timestamp/message construction. Coinbase total MUST
NOT exceed subsidy plus fees; maturity is network-bound.

## 11. Block format

```text
BlockHeader=i32 version||Hash256 previous_block_id||Hash256 merkle_root||u32 time||u32 bits||u32 nonce
Block=BlockHeader||array<transactions>
```

Headers are exactly 80 bytes. Blocks have 1..256 transactions, one first
coinbase, no other coinbase, no duplicate txids, and at most 1,000,000 bytes.
Header/transaction versions MUST NOT activate alternate v0 consensus rules.

## 12. Block hash

The block ID is `hash256` of exactly the Section 11 header bytes. Transactions
are committed through the Merkle root, not directly by the header hash.
`blocks.ncpv` binds TESTNET block serialization and header hash.

## 13. Merkle tree

Start with txids in order. A one-tx block root is its txid. At each level,
duplicate the final hash when count is odd, then replace adjacent pairs with
`hash256(left||right)`. The result MUST equal the header root. Reordering or
changing txids changes the root. `merkle.ncpv` covers the odd rule.

## 14. Proof of work

`bits` is Bitcoin compact target encoding (8-bit exponent and 23-bit unsigned
mantissa). Zero, negative/sign-bit, overflowing, non-canonical, or above-limit
targets reject. The raw block ID interpreted as unsigned little-endian MUST be
`<= target`; equality is valid. Operations use exact integers only.
`target_pow.ncpv` binds a canonical conversion.

Let `e=bits>>24` and `m=bits & 0x007fffff`. The sign bit `0x00800000`
MUST be clear. Decode `T=m>>(8*(3-e))` when `e<=3`, otherwise
`T=m<<(8*(e-3))`, using mathematical integers before checking 256-bit range.
Reject zero or overflow. To encode positive T, let e be its byte-length and
retain its three most significant bytes (right-pad with zeros if e<3).
If the mantissa high bit is set, shift it right eight bits and increment e.
Return `(e<<24)|mantissa`. Discarded low bytes are truncated, never rounded up.
Decoding MUST reject unless encoding T reproduces bits exactly, and T MUST NOT
exceed the selected network's existing powLimit.

## 15. Difficulty adjustment

Genesis uses committed bits. With `no_retargeting`, children inherit parent
expected bits. Otherwise, at a retarget boundary, clamp elapsed time to
`[timespan/4,timespan*4]`, then calculate
`min(powLimit, old_target*elapsed/timespan)` using checked wide integers and
truncation toward zero; canonically compact-encode it. REGTEST is
no-retargeting, TESTNET has 600-second spacing and its parameter-bound
minimum-difficulty behavior. MAINNET is **UNFINALIZED**. Floating point is
forbidden.

A boundary has `child_height % adjustment_interval == 0`; the first ancestor
is at `child_height-adjustment_interval`. Compute elapsed as
`max(parent.time-first.time,0)`, then clamp as above. A quotient exceeding
256-bit representation saturates at powLimit. Invalid parameters, invalid
previous targets and unavailable required ancestry are context failures, not
permission to substitute a target. Before freeze, evidence MUST establish that
other scaling failure causes cannot be confused with quotient saturation under
admitted parameters. That proof remains outstanding.

For TESTNET's existing minimum-difficulty rule outside retarget boundaries, a
child delayed strictly more than twice spacing (1200 seconds) uses powLimit.
Otherwise walk backward through consecutive powLimit ancestors, stopping at a
retarget boundary, a non-powLimit ancestor, or a powLimit ancestor that was NOT
delayed strictly more than twice spacing relative to its own predecessor.
Reuse the retained ancestor's expected bits. Recovery is defined over complete
validated ancestry. Missing ancestry is unavailable context, not an alternate
valid target or permanent invalidity of the block. ChainState/Synchronizer
missing-predecessor equivalence remains unverified. Current immutable-profile
vectors do not discriminate every recovery path; no new profile is introduced.

The no-retarget inheritance rule remains normative. Synchronizer's powLimit
shortcut is a known conformance gap requiring a separate alignment proposal;
coincidence on frozen REGTEST history is not a general equivalence proof.

## 16. Timestamp rules

Non-genesis time MUST exceed the lower median of configured prior header times
and MUST NOT exceed `validation_time+future_limit`. The median uses integers;
validation time is sampled from a trusted local clock before validation. Peer
data MUST NOT alter it. Local clock acquisition is policy; bounds are consensus.
Before addition, `validation_time > UINT32_MAX-future_limit` MUST reject the
validation context. Neither wrapping nor saturation is permitted. At equality
the addition is representable. Any `header.time+1` bookkeeping is a distinct
operation and MUST NOT substitute for this checked-addition rule.

## 17. Monetary policy

`COIN=100000000`, `INITIAL_SUBSIDY=50*COIN`, `MAX_MONEY=31000000*COIN`.
At height `h`, subsidy is `floor(INITIAL_SUBSIDY/2^floor(h/I))`; all shifts,
products, and totals are checked and the final nonzero era pays one unit.

| Network | interval | height 0 | first halving | exact theoretical full schedule |
| --- | ---: | ---: | ---: | ---: |
| REGTEST | 150 | 5000000000 | 2500000000 at 150 | 14,999.99998350 NOVA |
| TESTNET | 210000 | 5000000000 | 2500000000 at 210000 | 20,999,999.97690000 NOVA |
| MAINNET | **UNFINALIZED** | **UNFINALIZED** | **UNFINALIZED** | **UNFINALIZED** |

The integer sum `sum(k=0..63, floor(5000000000/2^k))` is 9,999,999,989
base units. Multiplying by the existing intervals gives 1,499,999,998,350
(REGTEST) and 2,099,999,997,690,000 (TESTNET) base units. Zero subsidy follows
the final nonzero era. These full schedules include the ordinary height-zero
term; they are not measured issued supply.

Parameter validation conservatively adds the full genesis allocation to the
full schedule: TESTNET's 1,000,000,000,000,000 genesis units plus
2,099,999,997,690,000 is 3,099,999,997,690,000, below MAX_MONEY
3,100,000,000,000,000. Actual child rewards start at height 1 and may be
underclaimed. Fees transfer existing value rather than create supply. Genesis
initialization, actual issuance and UTXO value MUST be accounted separately;
REGTEST starts with empty UTXOs while TESTNET seeds its frozen creator output.
No subsidy, allocation, interval or MAX_MONEY value changes. MAINNET economics
remain UNFINALIZED.

## 18. Block validation

Nodes SHALL: canonically parse/bound bytes; validate parent/time/target/PoW;
validate structure/coinbase/txids/Merkle root; validate regular transactions in
order against temporary UTXOs; validate fees and reward; and atomically publish
only valid state selected by chainwork. These are logical validation dependencies
and publication conditions, not a mandatory internal execution or error order.
Safe early checks and work calculation MAY occur without bypassing validation;
high work alone MUST NOT activate a candidate. Received, persisted, and local
blocks require all the same validation dependencies.

## 19. Chain selection

Block work is `floor(2^256/(target+1))`; chainwork is checked accumulated work.
Choose greatest valid cumulative work, never greatest height. Equal work uses
lexicographically smaller raw block ID. Overflow is invalid.

## 20. Reorganizations

Find the fork, disconnect the active suffix with undo data, connect the winning
suffix through Section 18, then publish the tip. Disconnect notifications occur
before connects. Failure MUST restore the old branch and UTXO state atomically.
This unconditional safety requirement is retained, not weakened for allocation,
crash or restoration failure. Implementation compliance remains FAIL pending
failure-model evidence and separately authorized remedies. A later design must
identify the observable publication/notification interface; ordinary successful
reorg fixtures do not prove all failure classes.

## 21. Mempool policy

Admission, relay, fees, eviction, capacity, and template ordering are
**NON-CONSENSUS**. Mempool presence MUST NOT bypass block validation.

## 22. P2P protocol

Envelope: `u32le magic||command[12]||u32le payload_length||checksum[4]||payload`.
Command is NUL-padded ASCII; checksum is first four raw hash256 bytes. Reject
wrong magic, malformed command, unknown command, excess or malformed complete payload,
checksum mismatch, and non-canonical payload. Supported commands are `version`,
`verack`, `ping`, `pong`, `addr`, `getaddr`, `inv`, `getdata`, `tx`, `block`,
`getheaders`, `headers`. Limits are user agent 256, addresses 1000, inventory
50000, locators 101, headers 2000. These are compatibility/resource policy;
parsed blocks/txs still require consensus validation. Incomplete stream fragments
MAY be buffered within existing bounds; an incomplete TCP fragment is not itself
a malformed complete message. No incomplete message may be dispatched as valid.
Connection close/timeout handling is separate local transport policy, not a new
consensus rule. See `docs/p2p.md` for the approved profile and transport bindings;
these clarifications select no new timeout or payload values.

## 23. Node synchronization

After handshake, discover headers from a verified tip, validate linkage,
difficulty, and PoW before accumulating work, request blocks, validate through
Section 18, then activate only the best valid chain. Advertised height is not
authority.

## 24. Network parameters

| Field | REGTEST | TESTNET | MAINNET |
| --- | --- | --- | --- |
| magic | `0xDAB5BFFA` | `0xDAB5BFFB` | **UNFINALIZED** |
| P2P/RPC | 18444/18443 | 28333/28332 | **UNFINALIZED** |
| P2PKH prefix | 111 | 112 | **UNFINALIZED** |
| pow limit | `0x207fffff` | `0x2070ffff` | **UNFINALIZED** |
| spacing | 600 s | 600 s | **UNFINALIZED** |
| halving | 150 | 210000 | **UNFINALIZED** |

Missing/invalid parameters MUST NOT default into an admitted network or state.
An internal locked sentinel is permitted only with safe rejection/non-admission
proof for every caller; unverified callers remain open. An internal fallback
lookup is not permission to initialize storage, chains or services. MAINNET
remains locked; the caller proof is not established by this specification.

## 25. Genesis block

Enabled genesis has zero parent, version 1, one coinbase, canonical Merkle
root, committed time/bits/nonce, valid PoW, and exact bytes. Its coinbase is
little-endian timestamp plus printable ASCII message, per Section 10 exception.

| Network | time | message | reward | nonce | hash |
| --- | ---: | --- | ---: | ---: | --- |
| REGTEST | 1704067200 | `NovaCoin Regtest Genesis` | 5000000000 | 3 | `c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79` |
| TESTNET | 1704153600 | `NovaCoin Testnet Genesis` | 1000000000000000 | 2 | `7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c` |
| MAINNET | **UNFINALIZED** | **UNFINALIZED** | **UNFINALIZED** | **UNFINALIZED** | **UNFINALIZED** |

## 26. Protocol versioning

P2P protocol/minimum-peer versions are transport compatibility only. They MAY
negotiate supported messages but MUST NOT select v0 consensus rules. REGTEST
and TESTNET use 1/1; MAINNET is **UNFINALIZED**.

## 27. Consensus upgrades

v0 has no activation mechanism. Software, tx, block, P2P version, peer signal,
or time MUST NOT silently activate alternate consensus. A future change needs a
separate normative specification, activation/rollback and fork analysis,
testnet evidence, vectors, external review, and explicit human approval.

## 28. Security invariants

Valid transactions cannot create money; UTXOs cannot be double-spent on a valid
chain; rewards cannot exceed subsidy plus fees; only valid-PoW chains activate;
selection uses cumulative work; and hostile inputs cannot bypass canonical,
bounded validation.

## 29. Test vectors

`test/data/protocol/` is canonical UTF-8 ASCII `NCPV1`: sorted unique
`name=value` fields followed by `.`; bytes are lowercase hex and decimal is
canonical. `MANIFEST.sha256` binds all `.ncpv` files. Unknown kinds or malformed
grammar MUST fail closed. Existing corpus covers hashing, transaction
serialization, SIGHASH_ALL, Merkle, compact target, subsidy, block
serialization, TESTNET genesis, and scoped P2PKH signature authorization.
Address and difficulty-example vectors and exhaustive signature coverage remain
explicitly insufficient, not inferred.

## 30. Implementation conformance

`nova.protocol_conformance` verifies manifest/grammar and C++ hashing, txid,
signature digest, scoped signature authorization with unchanged controlled UTXOs,
Merkle, compact target, subsidy, TESTNET block, and genesis vectors.
`docs/protocol-conformance-report.md` classifies every feature as
`IMPLEMENTED AND TESTED`, `IMPLEMENTED BUT INSUFFICIENTLY TESTED`, `SPECIFIED
BUT NOT IMPLEMENTED`, `IMPLEMENTED BUT NOT SPECIFIED`, or `INCONSISTENT`.
This specification does not freeze consensus, close readiness gates, launch a
network, or remove `MAINNET_LOCKED`.
