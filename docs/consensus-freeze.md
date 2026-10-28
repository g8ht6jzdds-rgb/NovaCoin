# Consensus freeze: checklist and change control

Status: **NOT FROZEN — HUMAN REVIEW REQUIRED**. Historical reviewed engineering revision:
`3846789a37ef28f5397090abbabb4260a80eb90c` (tree
`87a7b4eb3de1b4642614aca42b0e3cd9174db39f`); TASK022 follow-up changes must be
bound by patch/tree digest in evidence. This document does not approve a consensus
change. Frozen TESTNET genesis bytes remain unchanged; no final MAINNET genesis.

## Specification inventory

The stable clause/rejection index is in `consensus-clause-ledger.md`. Its
diagnostic-only `nova-freeze-corpus` companion emits canonical REGTEST/TESTNET
compiled-parameter and vector evidence to stdout from clean or archive-declared
source provenance. Neither artifact is a freeze acceptance or an activation
path.

| Topic | Normative location in protocol.md | Freeze finding |
| --- | --- | --- |
| Serialization/endian/varint/trailing bytes | 2 | Defined; exact bounds and schema implementation mapping still require sign-off |
| Transactions/UTXO and atomicity | 5–10, 18, 20 | Ordered temporary view, forward-reference rejection, coinbase timing and collision rule selected; complete evidence remains required |
| Signature verification/DER/low-S/sighash | 3, 8 | Defined; maintained-library version/behavior and adversarial boundaries reviewed externally |
| Coinbase/height/maturity | 6, 10, 17 | TESTNET maturity-zero genesis behavior explicitly distinct from MAINNET pending choice |
| Block/Merkle format | 11–13 | Exact bytes/odd duplication/duplicate txids defined |
| PoW/difficulty/timestamps | 14–16, 19 | Implemented work equation and no-retarget inheritance selected; exact discriminator vectors and sign-off remain required |
| Issuance/fees/supply | 5–10, 17 | Defined for current networks; MAINNET genesis allocation and total issuance NOT FINAL |
| Chain selection/reorganization | 19–20, chain-selection.md | Exact work/tie/rollback defined; verify deep cases plus signed-spend controls |
| Network upgrades | 26–27 | Immutable-v0 policy selected: no activation mechanism; future change process still requires human review |

## Known freeze blockers (not silently repaired)

The D5 owner decision selects an implementation-preserving parameter ownership
map: v0 has no general script interpreter; the exact P2PKH spend template and
serialized-size bound are the current resource contract; and block weight is a
serialized-size alias, not a separately implemented rule. The mapping still
requires exact-source/vector sign-off before freeze acceptance. It does not
authorize new script rules or parameter rewiring.

MAINNET's existing compiled table/placeholder genesis is NOT FINAL. A later
decision must resolve genesis distribution, maturity, issuance and parameters;
do not relabel the provisional fixture as a final commitment. D7 selects an
immutable-v0 policy, not an activation mechanism. These remain freeze blockers,
not permission to change consensus now.

## Freeze acceptance procedure

1. Identify every MUST and validation rejection, exact immutable parameters and
   supported parsing scope. Verify `consensus-clause-ledger.md`,
   `consensus-coverage.md`, and the hash-bound corpus rule/test mappings.
2. Resolve discrepancies in protocol FIRST, with rationale, economic/compatibility
   analysis and positive/negative/boundary vectors. Consensus independent of RPC,
   transport, wallet, UI and persistence; deterministic checked integer arithmetic.
3. External reviewers and designated consensus owner personally sign off on exact
   spec hash/source tree/parameter/corpus hashes. Record shared-control limitations.
4. Record accepted change process: proposed rule diff, threat model, migrations,
   version/activation decision, review, independent vectors, determinism, testnet
   upgrade/reorg/rollback evidence and a NEW freeze. Peers never choose rule sets.
5. Any consensus-affecting change invalidates affected gate acceptance and freeze;
   do not carry old PASS automatically. Gate acceptance is not launch permission.

No signature or human freeze acceptance has been supplied by this implementation.

## Current binding batch; not acceptance

Baseline `1b83e00f0e49a8703d0a6fef1291ccc538ca732e`; the topic locations above
now refer to the 30-section protocol. [Clarification evidence](consensus-evidence-bindings.json)
retains exact source/assertion/vector hashes and incomplete classifications.
No historical corpus acceptance is rebound to this revision. In particular,
CFD07 helper arithmetic is safe for positive operands, but the generic timing
predicate permits timespans 1–3 whose zero-clamped multiplier can be mistaken
for saturation. This static domain gap does not show frozen TESTNET reaches it.
No remedy, new parameter/profile or new acceptance is supplied by this batch.
Gate 1 remains **FAIL / NOT FROZEN**; Gate 2 incomplete; MAINNET locked.
