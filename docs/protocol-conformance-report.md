# NovaCoin protocol conformance report

**Candidate:** `4312ccfbd15efffbf340b7f831c1663d511ed4d7`
**Scope:** TASK025 initial vector corpus and test-only conformance target.  This
report does not resolve a disagreement or declare MAINNET readiness.

P2PKH corpus follow-up baseline: `33a448d25308161692ed1339689463a6a4e148b3`.
The scoped test/data/documentation delta adds six `signature_auth_*.ncpv`
records; the PR's exact head binds their manifest and replay assertions.

| Protocol feature | Evidence | Classification |
| --- | --- | --- |
| 1 Terminology | `docs/protocol.md` | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 2 Primitive serialization | `serialization_tests.cpp`; corpus grammar | IMPLEMENTED AND TESTED |
| 3 Hash/key/signature primitives | crypto/validation tests; hashing corpus | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 4 Addresses | `address_tests.cpp`; no fixed public address vector | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 5 Transactions | transaction tests; `transactions.ncpv` | IMPLEMENTED AND TESTED |
| 6 Inputs | validation tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 7 Outputs | transaction/validation tests | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
| 8 Signature authorization | validation tests; `sighash.ncpv`; six `signature_auth_*.ncpv` records replayed by `ProtocolConformance.SignatureAuthorizationCorpus`, with digest and push-boundary assertions | IMPLEMENTED BUT INSUFFICIENTLY TESTED |
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
- Address and difficulty-example vector families are not yet present. Scoped
  full P2PKH authorization is now present (valid, selector rejection, key-hash
  mismatch, digest mismatch, lower/upper push boundaries), but exhaustive
  signature/DER/key and multi-input coverage remains incomplete. TESTNET
  minimum-difficulty recovery remains IMPLEMENTED BUT INSUFFICIENTLY TESTED;
  this change adds no difficulty profile or vector.
- Gate 1 remains **FAIL / NOT FROZEN** pending human/external freeze review;
  Gate 2 is incomplete. Existing CTest-registration and clang-tidy limitations
  remain limitations, not passes.
- Any future code/spec/vector contradiction MUST be added as `INCONSISTENT`.

## CFD01–CFD15 / CFP01 approved clarification follow-up

Baseline `0ecaec5ef299859cb76e90060cf17798aba6f711`; see
[the decision-to-assertion trace](consensus-decision-coverage.md) for exact
positive, negative and boundary bindings and explicit missing evidence.
The original candidate references above are historical, not this follow-up's
tested-head identity. No existing corpus hash or production behavior changes.

Retarget exceptional-case proof, recovery adapter equivalence, unconditional
failure atomicity, every-caller sentinel safety and Synchronizer no-retarget
alignment remain open. The latter is a known conformance gap, not waived by
ordinary REGTEST outcomes. Gate 1 remains FAIL / NOT FROZEN, Gate 2 incomplete.
Historical failures and unavailable checks are not converted to passes.

## Scoped signature triad follow-up

Base `1b83e00f0e49a8703d0a6fef1291ccc538ca732e`: eight additive public-fixture
records cover invalid key prefix, DER tag, trailing authorization, adjacent push
lengths, unsupported referenced lock and amount equality/insufficiency. The
original fourteen corpus files remain byte-identical. Exact clause-to-triad and
named assertion bindings are in `consensus-coverage.md`; record grammar and
single-fault mutations are in `test/data/protocol/FORMAT.md`.

The conformance harness checks the independently serialized golden preimage and
the unchanged digest of amount-only synthetic UTXO contexts, plus exact fees and
non-mutation of both controlled coins. These assertions do not prove alternative
valid chain histories or block/reorg atomicity. Historical IMPLEMENTED AND TESTED
family labels above remain scoped historical evidence, not complete current
Gate 2 certificates. This follow-up adds no normative rule or production change.

Maximum-valid DER71/push72, complete key/DER/multi-input cases, full difficulty
histories, recovery discrimination, scaling failure classification, ancestry
handling, Synchronizer alignment, sentinel callers and unconditional atomicity
remain incomplete. Gate 1 is FAIL / NOT FROZEN; Gate 2 incomplete; MAINNET locked.
Actual exact-head checks, failures and unavailable-tool/platform limitations
belong in the accompanying external evidence packet, not presumed passes here.
