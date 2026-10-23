# Consensus freeze: checklist and change control

Status: **NOT FROZEN — HUMAN REVIEW REQUIRED**. Base engineering revision:
`cf4523f4cf8b2f4c53511f62fef5e215203dd096`; TASK022 working changes must be bound
by patch/tree digest in evidence. This document does not approve a consensus
change. Frozen TESTNET genesis bytes remain unchanged; no final MAINNET genesis.

## Specification inventory

| Topic | Normative location in protocol.md | Freeze finding |
| --- | --- | --- |
| Serialization/endian/varint/trailing bytes | 2–3 | Defined; exact bounds and schema implementation mapping require sign-off |
| Transactions/UTXO and atomicity | 5, 8, 10 | Defined; complete positive/negative/boundary matrix required |
| Signature verification/DER/low-S/sighash | 3, 7 | Defined; maintained-library version/behavior and adversarial boundaries reviewed externally |
| Coinbase/height/maturity | 5–6, 17 | TESTNET maturity-zero genesis behavior explicitly distinct from MAINNET pending choice |
| Block/Merkle format | 6 | Exact bytes/odd duplication/duplicate txids defined |
| PoW/difficulty/timestamps | 9 | Integer formula/clamps/MTP/future validation clock defined |
| Issuance/fees/supply | 4–6, 17 | Defined for current networks; MAINNET genesis allocation and total issuance NOT FINAL |
| Chain selection/reorganization | 10, chain-selection.md | Exact work/tie/rollback defined; verify deep cases plus signed-spend controls |
| Network upgrades | 20–21 | Intentionally UNIMPLEMENTED. No activation defaults; mechanism/compatibility decision required before real-value launch |

## Known freeze blockers (not silently repaired)

The normative section 4 schema lists `max_script_element_size`, `max_script_ops`
and `max_stack_items`; actual `NetworkParams`/BlockLimits do not expose all those
fields. Section 7 refers to those limits but uses an exact P2PKH template instead
of a general interpreter. The consensus owner must explicitly reconcile the
schema/template/resource-bound contract before claiming completeness. Do not
invent a new opcode interpreter or silently delete normative limits.
This is a freeze/specification-mapping deficiency, not demonstrated evidence
that the current exact-template validator accepts an invalid transaction.

MAINNET's existing compiled table/placeholder genesis is NOT FINAL. A versioned
decision must resolve genesis distribution, maturity, issuance and parameters;
do not relabel the provisional fixture as a final commitment. Network upgrades
need a normative accepted design or an explicit immutable-v0 compatibility policy.
These are specification/decision blockers, not permission to change consensus now.

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
