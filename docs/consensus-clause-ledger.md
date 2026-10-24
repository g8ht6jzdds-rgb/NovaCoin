# Consensus clause ledger (v0 evidence index)

Status: **evidence index only — NOT a consensus freeze**. This ledger is
bound by a reviewable source revision and the diagnostic corpus; it neither
selects protocol behavior nor supplies human freeze acceptance. `MAPPED` means
that a source location and named assertion have been identified, not that the
assertion is exhaustive. The other permitted outcomes are `UNMATCHED`,
`AMBIGUOUS`, `IMPLEMENTATION_DEPENDENT`, and `HUMAN_PROCESS`.

Stable IDs are never reused. Source/test references are repository-relative and
use named GTest assertions when one exists. A `HUMAN_PROCESS` row deliberately
has no implementation assertion: it requires reviewable operational or human
evidence rather than a fabricated automated pass.

| ID | Protocol clause | Classification | Source / evidence | Named assertion or required evidence |
| --- | --- | --- | --- | --- |
| NCV0-001 | §1 explorer results MUST NOT affect validation, selection, or UTXOs | MAPPED | `src/explorer/` | `Explorer*` boundary assertions |
| NCV0-002 | §1 explorer feed MUST be one-way and MUST NOT invoke transitions | MAPPED | `src/explorer/` | `Explorer*` boundary assertions |
| NCV0-003 | §1 peer, disk, and local blocks MUST use pure validation | MAPPED | `src/chain/validation.cpp` | `BlockValidation.*` |
| NCV0-004 | §1 integer arithmetic; floating point forbidden; overflow rejects | MAPPED | `src/consensus/`, `src/chain/` | `Monetary*`, `PowWork.*` |
| NCV0-005 | §2 CompactSize MUST be shortest and decoder MUST reject invalid forms | MAPPED | `src/primitives/serialization.cpp` | `SerializationCompactSize.*`, `SerializationMalformed.*` |
| NCV0-006 | §3 crypto MUST use reviewed library and MUST NOT implement primitives | HUMAN_PROCESS | dependency/audit records | external crypto/dependency review |
| NCV0-007 | §4 every validator MUST receive immutable `NetworkParams` | MAPPED | `src/chain/validation.*` | `NetworkParams.*` |
| NCV0-008 | §4 MAINNET values MUST be reviewed before enablement | HUMAN_PROCESS | `docs/protocol.md`, `MAINNET_CHECKLIST.md` | owner/external acceptance; MAINNET remains disabled |
| NCV0-009 | §4 MAINNET MUST remain non-final and disabled | MAPPED | `src/consensus/network_params.cpp`, `src/node/` | `MainnetLock.*`, `NetworkParams.*` |
| NCV0-010 | §4 TESTNET refusal policy MUST hold while non-final/disabled | MAPPED | `src/consensus/network_params.cpp`, `src/node/` | `NetworkSelection.*` |
| NCV0-011 | §5 ordinary transaction MUST have bounded inputs/outputs/amounts | MAPPED | `src/primitives/transaction.cpp` | `TransactionStructure.*` |
| NCV0-012 | §5 coinbase MUST satisfy null-input, placement, size, and height rules | MAPPED | `src/primitives/block.cpp`, `src/chain/validation.cpp` | `BlockStructure.*`, `MainnetConsensusBoundary.*` |
| NCV0-013 | §5 every input MUST exist in ordered temporary UTXO view | MAPPED | `src/chain/validation.cpp` | `BlockValidation.AllowsOrderedRegularDependenciesAndRejectsForwardReferences` |
| NCV0-014 | §5 no outpoint may be spent twice | MAPPED | `src/chain/validation.cpp` | `BlockValidation.*Duplicate*` |
| NCV0-015 | §5 coinbase maturity MUST be enforced | MAPPED | `src/chain/validation.cpp` | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` |
| NCV0-016 | §5 sums MUST be checked; inputs MUST cover outputs | MAPPED | `src/chain/validation.cpp` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` |
| NCV0-017 | §5 unlocking scripts MUST satisfy supported lock | MAPPED | `src/chain/validation.cpp` | `BlockValidation.RejectsUnsupportedReferencedLockingScriptWithoutMutation` |
| NCV0-018 | §5 transaction MUST be final | MAPPED | `src/chain/validation.cpp` | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` |
| NCV0-019 | §6 non-genesis blocks MUST have known parent and valid coinbase bounds | MAPPED | `src/chain/validation.cpp` | `BlockValidation.RejectsPreviousBlockMismatchBeforeStateMutation` |
| NCV0-020 | §6 merkle root MUST equal ordered transaction commitment | MAPPED | `src/primitives/block.cpp` | `BlockMerkle.*`, `BlockStructure.*` |
| NCV0-021 | §6 coinbase total MUST not exceed subsidy plus fees | MAPPED | `src/consensus/monetary.cpp` | `MonetaryCoinbaseReward.*` |
| NCV0-022 | §7 signature/key checks MUST be strict and supported P2PKH only | MAPPED | `src/crypto/keys.cpp`, `src/chain/validation.cpp` | `CryptoParsing.*`, `SignatureHash.*` |
| NCV0-023 | §9 expected bits MUST be derived; adapters MUST NOT provide them | MAPPED | `src/chain/chainstate.cpp` | `ChainStateDifficulty.*` |
| NCV0-024 | §9 parameter validation MUST reject invalid retarget inputs | MAPPED | `src/consensus/network_params.cpp` | `MonetaryParameters.*`, `NetworkParams.*` |
| NCV0-025 | §9 time MUST exceed MTP and MUST obey future bound | MAPPED | `src/chain/chainstate.cpp` | `ChainStateSecurity.*`, `MainnetConsensusBoundary.*` |
| NCV0-026 | §9 rejected header MUST NOT alter local time/state | MAPPED | `src/chain/chainstate.cpp` | `ChainStateSecurity.RejectsFarFutureHeadersWithoutChangingTheActiveTip` |
| NCV0-027 | §10 best branch MUST apply/revert atomically and publish only on success | IMPLEMENTATION_DEPENDENT | `src/chain/chainstate.cpp` | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` |
| NCV0-028 | §10 persistence failure MUST roll accepted block back | IMPLEMENTATION_DEPENDENT | `src/storage/`, `src/chain/` | restart/recovery evidence remains Gate 6 |
| NCV0-029 | §10 inactive blocks MUST NOT be reported confirmed | MAPPED | `src/chain/chainstate.cpp`, explorer | `ChainStateSelection.*`, `Explorer*` |
| NCV0-030 | §11 parser MUST bound frame/length/allocation/recursion and reject bad counts | MAPPED | `src/net/`, `src/primitives/serialization.cpp` | `SerializationMalformed.*`, `P2P*` |
| NCV0-031 | §11 consensus errors MUST NOT mutate valid state | MAPPED | `src/chain/validation.cpp` | `BlockValidation.*WithoutMutation` |
| NCV0-032 | §13 received work MUST use canonical validation; transport MUST NOT mutate chain | MAPPED | `src/net/`, `src/chain/` | `NetworkInitialization.*`, `SyncProgress.*` |
| NCV0-033 | §15 wallet MUST NOT decide or mutate consensus state | MAPPED | `src/wallet/` | `Wallet*` boundary assertions |
| NCV0-034 | §16 RPC MUST authenticate/bind/bound requests and MUST NOT export keys | MAPPED | `src/rpc/` | `RpcIntegration.*` |
| NCV0-035 | §19 every rule MUST have deterministic accepted/rejected vectors | UNMATCHED | `docs/consensus-coverage.md` | Gate 2 coverage matrix remains incomplete |
| NCV0-036 | §19 fuzz targets MUST be bounded and reject malformed input safely | UNMATCHED | `fuzz/` | Gate 8 fuzz evidence required |
| NCV0-037 | §20 implementation MUST expose no default public network | MAPPED | `src/consensus/network_params.cpp`, `src/node/` | `NetworkSelection.*`, `MainnetLock.*` |
| NCV0-038 | §21 MAINNET lock MUST prevent construction before side effects | MAPPED | `src/node/mainnet_lock.*`, `src/daemon/` | `MainnetLock.*` |
| NCV0-039 | §21 versions MUST NOT select alternative v0 consensus rules | MAPPED | `src/primitives/`, `src/net/` | `VersionPolicy.*` |
| NCV0-040 | §21 future consensus change MUST have separate design/review/approval | HUMAN_PROCESS | `docs/consensus-freeze.md` | designated owner and external-review acceptance |

## Rejection ledger

The following rejection families are independently indexed because a rejection
is evidence-bearing even where the surrounding paragraph contains no uppercase
normative word: `R01` noncanonical/truncated serialization (`SerializationMalformed.*`),
`R02` transaction bounds/value/duplicate input (`TransactionStructure.*`),
`R03` UTXO/maturity/finality/signature failure (`BlockValidation.*`), `R04`
block/merkle/coinbase failure (`BlockStructure.*`), `R05` compact target/PoW/
difficulty/time failure (`Pow*`, `ChainStateSecurity.*`), `R06` chain-work/
reorganization atomicity (`ChainStateSelection.*`, `MainnetReadiness.*`), and
`R07` network parser limits (`P2P*`, fuzz evidence pending). The exact
parameter-and-vector values for REGTEST and TESTNET are emitted by
`nova-freeze-corpus --regtest-testnet` only from clean or archive-declared
source provenance.

This ledger is deliberately conservative. `UNMATCHED`,
`IMPLEMENTATION_DEPENDENT`, and `HUMAN_PROCESS` rows prevent it from being used
as a Gate 1 PASS or freeze-acceptance substitute.
