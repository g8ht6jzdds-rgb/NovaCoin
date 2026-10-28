# Consensus clause ledger (v0 evidence index)

Status: **evidence index only — NOT a consensus freeze**.

Current-location crosswalk baseline: `1b83e00f0e49a8703d0a6fef1291ccc538ca732e`.
Legacy requirement wording/IDs below is retained; architecture, RPC and activation
rows are identified separately where the current protocol has no direct sentence.
This crosswalk does not create new protocol requirements or promote old MAPPED rows.
The original section references remain in Git history.

Nine approved clarification bindings are machine-readable in
[`consensus-evidence-bindings.json`](consensus-evidence-bindings.json). Each binds
quoted protocol text, LF-normalized source/test SHA-256 and line/token, three explicitly
described cases, and existing vector provenance where applicable. All remain
`MAPPED_BUT_TRIAD_INCOMPLETE`: a shared test name or verified hash is not proof
of exhaustive semantics. `scripts/test_consensus_evidence.py` checks drift and
malformed evidence, not consensus correctness or human acceptance. This ledger is
bound by a reviewable source revision and the diagnostic corpus; it neither
selects protocol behavior nor supplies human freeze acceptance. Historical `MAPPED` meant
that a source location and named assertion have been identified, not that the
assertion is exhaustive. Current mapped rows are conservatively `MAPPED_BUT_TRIAD_INCOMPLETE`. Other outcomes include `UNMATCHED`,
`AMBIGUOUS`, `IMPLEMENTATION_DEPENDENT`, and `HUMAN_PROCESS`.

Stable IDs are never reused. Source/test references are repository-relative and
use named GTest assertions when one exists. A `HUMAN_PROCESS` row deliberately
has no implementation assertion: it requires reviewable operational or human
evidence rather than a fabricated automated pass.

| ID | Protocol clause | Classification | Source / evidence | Named assertion or required evidence |
| --- | --- | --- | --- | --- |
| NCV0-001 | Current Architecture evidence (no direct current protocol sentence): explorer results MUST NOT affect validation, selection, or UTXOs | MAPPED_BUT_TRIAD_INCOMPLETE | `src/explorer/` | `Explorer*` boundary assertions |
| NCV0-002 | Current Architecture evidence (no direct current protocol sentence): explorer feed MUST be one-way and MUST NOT invoke transitions | MAPPED_BUT_TRIAD_INCOMPLETE | `src/explorer/` | `Explorer*` boundary assertions |
| NCV0-003 | Current 18: peer, disk, and local blocks MUST use pure validation | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `BlockValidation.*` |
| NCV0-004 | Current 2/15/17: integer arithmetic; floating point forbidden; overflow rejects | MAPPED_BUT_TRIAD_INCOMPLETE | `src/consensus/`, `src/chain/` | `Monetary*`, `PowWork.*` |
| NCV0-005 | Current 2: CompactSize MUST be shortest and decoder MUST reject invalid forms | MAPPED_BUT_TRIAD_INCOMPLETE | `src/primitives/serialization.cpp` | `SerializationCompactSize.*`, `SerializationMalformed.*` |
| NCV0-006 | Current 3: crypto MUST use reviewed library and MUST NOT implement primitives | HUMAN_PROCESS | dependency/audit records | external crypto/dependency review |
| NCV0-007 | Parameter ownership evidence (24; retained legacy requirement): every validator MUST receive immutable `NetworkParams` | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.*` | `NetworkParams.*` |
| NCV0-008 | Current 24/25: MAINNET values MUST be reviewed before enablement | HUMAN_PROCESS | `docs/protocol.md`, `MAINNET_CHECKLIST.md` | owner/external acceptance; MAINNET remains disabled |
| NCV0-009 | Current Status/24: MAINNET MUST remain non-final and disabled | MAPPED_BUT_TRIAD_INCOMPLETE | `src/consensus/network_params.cpp`, `src/node/` | `MainnetLock.*`, `NetworkParams.*` |
| NCV0-010 | Current 24: TESTNET refusal policy MUST hold while non-final/disabled | MAPPED_BUT_TRIAD_INCOMPLETE | `src/consensus/network_params.cpp`, `src/node/` | `NetworkSelection.*` |
| NCV0-011 | Current 5: ordinary transaction MUST have bounded inputs/outputs/amounts | MAPPED_BUT_TRIAD_INCOMPLETE | `src/primitives/transaction.cpp` | `TransactionStructure.*` |
| NCV0-012 | Current 6/10/11: coinbase MUST satisfy null-input, placement, size, and height rules | MAPPED_BUT_TRIAD_INCOMPLETE | `src/primitives/block.cpp`, `src/chain/validation.cpp` | `BlockStructure.*`, `MainnetConsensusBoundary.*` |
| NCV0-013 | Current 6/9: every input MUST exist in ordered temporary UTXO view | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `BlockValidation.AllowsOrderedRegularDependenciesAndRejectsForwardReferences` |
| NCV0-014 | Current 6/9: no outpoint may be spent twice | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `BlockValidation.*Duplicate*` |
| NCV0-015 | Current 6/10: coinbase maturity MUST be enforced | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` |
| NCV0-016 | Current 5/9: sums MUST be checked; inputs MUST cover outputs | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` |
| NCV0-017 | Current 7/8: unlocking scripts MUST satisfy supported lock | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `BlockValidation.RejectsUnsupportedReferencedLockingScriptWithoutMutation` |
| NCV0-018 | Current 6: transaction MUST be final | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` |
| NCV0-019 | Current 11/18: non-genesis blocks MUST have known parent and valid coinbase bounds | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/validation.cpp` | `BlockValidation.RejectsPreviousBlockMismatchBeforeStateMutation` |
| NCV0-020 | Current 13: merkle root MUST equal ordered transaction commitment | MAPPED_BUT_TRIAD_INCOMPLETE | `src/primitives/block.cpp` | `BlockMerkle.*`, `BlockStructure.*` |
| NCV0-021 | Current 10: coinbase total MUST not exceed subsidy plus fees | MAPPED_BUT_TRIAD_INCOMPLETE | `src/consensus/monetary.cpp` | `MonetaryCoinbaseReward.*` |
| NCV0-022 | Current 3/8: signature/key checks MUST be strict and supported P2PKH only | MAPPED_BUT_TRIAD_INCOMPLETE | `src/crypto/keys.cpp`, `src/chain/validation.cpp` | `CryptoParsing.*`, `SignatureHash.*` |
| NCV0-023 | Current 15: expected bits MUST be derived; adapters MUST NOT provide them | IMPLEMENTATION_DEPENDENT | `src/chain/chainstate.cpp` | `ChainStateDifficulty.*` |
| NCV0-024 | Current 15: parameter validation MUST reject invalid retarget inputs | IMPLEMENTATION_DEPENDENT | `src/consensus/network_params.cpp` | `MonetaryParameters.*`, `NetworkParams.*` |
| NCV0-025 | Current 16: time MUST exceed MTP and MUST obey future bound | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/chainstate.cpp` | `ChainStateSecurity.*`, `MainnetConsensusBoundary.*` |
| NCV0-026 | Current 16: rejected header MUST NOT alter local time/state | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/chainstate.cpp` | `ChainStateSecurity.RejectsFarFutureHeadersWithoutChangingTheActiveTip` |
| NCV0-027 | Current 9/18/20: best branch MUST apply/revert atomically and publish only on success | IMPLEMENTATION_DEPENDENT | `src/chain/chainstate.cpp` | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` |
| NCV0-028 | Current 20: persistence failure MUST roll accepted block back | IMPLEMENTATION_DEPENDENT | `src/storage/`, `src/chain/` | restart/recovery evidence remains Gate 6 |
| NCV0-029 | Current 1/19/20: inactive blocks MUST NOT be reported confirmed | MAPPED_BUT_TRIAD_INCOMPLETE | `src/chain/chainstate.cpp`, explorer | `ChainStateSelection.*`, `Explorer*` |
| NCV0-030 | Current 2/22: parser MUST bound frame/length/allocation/recursion and reject bad counts | MAPPED_BUT_TRIAD_INCOMPLETE | `src/net/`, `src/primitives/serialization.cpp` | `SerializationMalformed.*`, `P2P*` |
| NCV0-031 | Current 9/18/20: consensus errors MUST NOT mutate valid state | IMPLEMENTATION_DEPENDENT | `src/chain/validation.cpp` | `BlockValidation.*WithoutMutation` |
| NCV0-032 | Current 18/23: received work MUST use canonical validation; transport MUST NOT mutate chain | IMPLEMENTATION_DEPENDENT | `src/net/`, `src/chain/` | `NetworkInitialization.*`, `SyncProgress.*` |
| NCV0-033 | Current Architecture evidence (no direct current protocol sentence): wallet MUST NOT decide or mutate consensus state | MAPPED_BUT_TRIAD_INCOMPLETE | `src/wallet/` | `Wallet*` boundary assertions |
| NCV0-034 | Current RPC readiness evidence (no direct current protocol sentence): RPC MUST authenticate/bind/bound requests and MUST NOT export keys | MAPPED_BUT_TRIAD_INCOMPLETE | `src/rpc/` | `RpcIntegration.*` |
| NCV0-035 | Current 29/30: every rule MUST have deterministic accepted/rejected vectors | UNMATCHED | `docs/consensus-coverage.md` | Gate 2 coverage matrix remains incomplete |
| NCV0-036 | Current Parser readiness evidence (22/28): fuzz targets MUST be bounded and reject malformed input safely | UNMATCHED | `fuzz/` | Gate 8 fuzz evidence required |
| NCV0-037 | Current Activation readiness evidence (Status/24): implementation MUST expose no default public network | MAPPED_BUT_TRIAD_INCOMPLETE | `src/consensus/network_params.cpp`, `src/node/` | `NetworkSelection.*`, `MainnetLock.*` |
| NCV0-038 | Current Status/24: MAINNET lock MUST prevent construction before side effects | MAPPED_BUT_TRIAD_INCOMPLETE | `src/node/mainnet_lock.*`, `src/daemon/` | `MainnetLock.*` |
| NCV0-039 | Current 5/11/26/27: versions MUST NOT select alternative v0 consensus rules | MAPPED_BUT_TRIAD_INCOMPLETE | `src/primitives/`, `src/net/` | `VersionPolicy.*` |
| NCV0-040 | Current 27: future consensus change MUST have separate design/review/approval | HUMAN_PROCESS | `docs/consensus-freeze.md` | designated owner and external-review acceptance |

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
