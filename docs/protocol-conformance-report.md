# NovaCoin protocol conformance report

**Candidate:** `4312ccfbd15efffbf340b7f831c1663d511ed4d7`
**Scope:** TASK025 initial vector corpus and test-only conformance target.  This
report does not resolve a disagreement or declare MAINNET readiness.

| Protocol feature | Evidence | Classification |
| --- | --- | --- |
| 1 Terminology | `docs/protocol.md` | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 2 Primitive serialization | `serialization_tests.cpp`; corpus grammar | IMPLEMENTED AND TESTED |
| 3 Hash/key/signature primitives | crypto/validation tests; hashing corpus | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 4 Addresses | `address_tests.cpp`; no fixed public address vector | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 5 Transactions | transaction tests; `transactions.ncpv` | IMPLEMENTED AND TESTED |
| 6 Inputs | validation tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 7 Outputs | transaction/validation tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 8 Signature authorization | validation tests; `sighash.ncpv` | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 9 UTXO model | validation/UTXO tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 10 Coinbase | validation/monetary/network-parameter tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 11 Block format | block tests; `blocks.ncpv` | IMPLEMENTED AND TESTED |
| 12 Block hash | block/network-parameter tests | IMPLEMENTED AND TESTED |
| 13 Merkle tree | block tests; `merkle.ncpv` | IMPLEMENTED AND TESTED |
| 14 Proof of work | PoW tests; `target_pow.ncpv` | IMPLEMENTED AND TESTED |
| 15 Difficulty | PoW/network-parameter tests; no public calculation corpus | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 16 Timestamps | validation boundary tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 17 Monetary policy | monetary tests; `subsidy.ncpv` | IMPLEMENTED AND TESTED |
| 18 Block validation | validation tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 19 Chain selection | chainstate tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 20 Reorganizations | chainstate/reorg tests; long-run Gate 5 pending | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 21 Mempool policy | mempool tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 22 P2P protocol | p2p tests; parser fuzzing/gate evidence incomplete | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 23 Synchronization | sync tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 24 Network parameters | network-parameter tests | IMPLEMENTED AND TESTED for REGTEST/TESTNET; MAINNET is UNFINALIZED |
| 25 Genesis | exact REGTEST/TESTNET tests and `genesis.ncpv` | IMPLEMENTED AND TESTED for REGTEST/TESTNET; MAINNET is UNFINALIZED |
| 26 Protocol versioning | transaction/block version tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 27 Consensus upgrades | immutable-v0 policy only | SPECIFIED BUT NOT IMPLEMENTED |
| 28 Security invariants | dispersed unit tests; no complete assertion trace | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 29 Machine-readable vectors | `test/data/protocol/` | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 30 Conformance suite | `nova.protocol_conformance` | IMPLEMENTED BUT INSUFFICIENTLY TESTED |

## Open limitations

- MAINNET values and genesis are **UNFINALIZED**; no conformance result applies
  to MAINNET.
- Address, full signature, and difficulty-example vector families are not yet
  present.
- Gate 1 remains **FAIL / NOT FROZEN** pending human/external freeze review;
  Gate 2 is incomplete. Existing CTest-registration and clang-tidy limitations
  remain limitations, not passes.
- Any future code/spec/vector contradiction MUST be added as `INCONSISTENT`.
