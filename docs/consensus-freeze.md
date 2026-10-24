# Consensus freeze: checklist and change control

Status: **NOT FROZEN — HUMAN REVIEW REQUIRED**. Base engineering revision:
`9387c8b03bf308710f6927acc318ca3f968cb2b9`; TASK022 working changes must be bound
by patch/tree digest in evidence. This document does not approve a consensus
change. Frozen TESTNET genesis bytes remain unchanged; no final MAINNET genesis.

## Specification inventory

| Topic | Normative location in protocol.md | Freeze finding |
| --- | --- | --- |
| Serialization/endian/varint/trailing bytes | 2–3 | Defined; exact bounds and schema implementation mapping still require sign-off |
| Transactions/UTXO and atomicity | 5, 8, 10 | Ordered temporary view, forward-reference rejection, coinbase timing and collision rule selected; complete evidence remains required |
| Signature verification/DER/low-S/sighash | 3, 7 | Defined; maintained-library version/behavior and adversarial boundaries reviewed externally |
| Coinbase/height/maturity | 5–6, 17 | TESTNET maturity-zero genesis behavior explicitly distinct from MAINNET pending choice |
| Block/Merkle format | 6 | Exact bytes/odd duplication/duplicate txids defined |
| PoW/difficulty/timestamps | 9 | Implemented work equation and no-retarget inheritance selected; exact discriminator vectors and sign-off remain required |
| Issuance/fees/supply | 4–6, 17 | Defined for current networks; MAINNET genesis allocation and total issuance NOT FINAL |
| Chain selection/reorganization | 10, chain-selection.md | Exact work/tie/rollback defined; verify deep cases plus signed-spend controls |
| Network upgrades | 20–21 | Immutable-v0 policy selected: no activation mechanism; future change process still requires human review |

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
   supported parsing scope. Verify consensus-coverage.md rule/test mappings.
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
