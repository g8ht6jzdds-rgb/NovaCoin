# Consensus rule coverage matrix

Evidence inventory, NOT completeness proof. Named tests contain multiple vectors;
review assertions rather than names. Paths are under src/. `NONE` identifies
missing dedicated coverage and blocks Gate 2. No case is skipped to obtain PASS.

| Rule | Implementation location | Positive test | Negative test | Boundary test |
| --- | --- | --- | --- | --- |
| C01 Fixed endian fields | primitives/serialization.cpp | `SerializationScalars.UsesExplicitLittleEndianEncoding` | `SerializationMalformed.RejectsTruncationNoncanonicalLengthsBoundsAndTrailingBytes` | `SerializationScalars.UsesExplicitLittleEndianEncoding` |
| C02 Minimal CompactSize/count/length | primitives/serialization.cpp | `SerializationCompactSize.UsesShortestCanonicalEncoding` | `SerializationMalformed.RejectsTruncationNoncanonicalLengthsBoundsAndTrailingBytes` | `SerializationCompactSize.CoversEveryWireWidthAndCallerBoundary` |
| C03 Canonical transaction bytes/IDs/trailing data | primitives/transaction.cpp | `TransactionSerialization.HasCanonicalByteLayoutAndStableTxId` | `TransactionParsing.RejectsMalformedAndNoncanonicalEncodings` | `TransactionStructure.RejectsLimitsAndOversizeProperties` |
| C04 Input/output bounds and uniqueness | primitives/transaction.cpp | `TransactionStructure.AcceptsAValidCoinbaseShapedTransaction` | `TransactionStructure.RejectsInvalidIndependentProperties` | `TransactionStructure.RejectsLimitsAndOversizeProperties` |
| C05 UTXO existence/double spend | chain/validation.cpp; chain/utxo.cpp | `BlockValidation.AllowsOrderedRegularDependenciesAndRejectsForwardReferences` | `BlockValidation.RejectsMissingUtxoBeforeSignatureChecking` | `UTXOApply.RejectsStagedAdditionCollisionAndRecoversDistinctOutputs` |
| C06 Signed value conservation and fees | chain/validation.cpp | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` |
| C07 Coinbase maturity | chain/validation.cpp | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` | `BlockValidation.RejectsImmatureCoinbaseAndInsufficientInputs` | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` |
| C08 Coinbase placement/count/size/height | primitives/block.cpp; chain/validation.cpp | `BlockStructure.SerializesParsesAndHashesCanonicalBlocks` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` | `MainnetConsensusBoundary.AcceptsMinimalOrdinaryCoinbaseHeightEncodings` |
| C09 Halving/final atomic era/zero | consensus/monetary.cpp | `MonetarySubsidy.HasExactIntegerValuesAcrossHalvingBoundaries` | `MonetaryParameters.RejectsInvalidIntervalsConstantsAndExcessIssuance` | `MonetarySubsidy.HasExactIntegerValuesAcrossHalvingBoundaries` |
| C10 Theoretical supply and genesis allocation | consensus/monetary.cpp; consensus/network_params.cpp | `MonetaryIssuance.TestnetCreatorAllocationAndScheduledIssuanceFitTheSupplyCap` | `NetworkParams.RejectsGenesisAllocationThatExceedsTheSupplyCap` | `MainnetReadiness.IssuanceChecksRejectOverflowAndOneUnitExcessReward` |
| C11 Reward/fee/amount overflow | consensus/monetary.cpp | `MonetaryCoinbaseReward.PermitsAtMostSubsidyPlusValidatedFees` | `MonetaryCoinbaseReward.RejectsInflationAndMalformedMonetaryInputs` | `MainnetReadiness.IssuanceChecksRejectOverflowAndOneUnitExcessReward` |
| C12 Merkle ordering/odd duplication/duplicate IDs | primitives/block.cpp | `BlockMerkle.DeterministicVectorsForOneTwoThreeAndOddCounts` | `BlockMerkle.ChangesForChangedTransactionAndOrdering` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` |
| C13 Header/block size and parent | primitives/block.cpp; chain/validation.cpp | `BlockStructure.SerializesParsesAndHashesCanonicalBlocks` | `BlockValidation.RejectsPreviousBlockMismatchBeforeStateMutation` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` |
| C14 Key/DER/low-S/ECDSA | crypto/keys.cpp; chain/validation.cpp | `CryptoSignatures.DeterministicSignAndVerify` | `CryptoParsing.RejectsMalformedKeysAndSignatures` | `CryptoParsing.RejectsMalformedKeysAndSignatures` |
| C15 P2PKH supported spends/SIGHASH_ALL | chain/validation.cpp | `BlockValidation.ConnectsAndDisconnectsOneSignedBlockAtomically` | `BlockValidation.RejectsUnsupportedReferencedLockingScriptWithoutMutation` | `SignatureHash.HasFixedPreimageAndDigestVector` |
| C16 Locktime/sequence | chain/validation.cpp | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` | `BlockValidation.RejectsTimestampCoinbaseHeightAndNonfinalTransactions` | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` |
| C17 Compact target canonicality/limit | consensus/pow.cpp | `PowCompactTarget.DecodesAndEncodesCanonicalTargets` | `PowCompactTarget.RejectsMalformedAndNonCanonicalEncodings` | `PowCompactTarget.HandlesBoundaryMantissasWithoutOutOfRangeAccess` |
| C18 Inclusive little-endian PoW | consensus/pow.cpp | `PowValidation.UsesAnInclusiveLittleEndianHashComparison` | `BlockValidation.RejectsProofOfWorkFailures` | `PowValidation.UsesAnInclusiveLittleEndianHashComparison` |
| C19 Retarget/clamps/min-difficulty/rounding | chain/chainstate.cpp | `SyncProgress.RetargetWalkSpansValidatedAndPendingMetadata` | `MainnetConsensusBoundary.RetargetLowerClampAndPowLimitUseValidatedAncestors` | `ChainStateDifficulty.NoRetargetingInheritsParentExpectedBits` |
| C20 MTP/future clock | chain/chainstate.cpp; chain/validation.cpp | `ChainStateSelection.ExtendsAStraightChainByCumulativeWork` | `ChainStateSecurity.RejectsFarFutureHeadersWithoutChangingTheActiveTip` | `MainnetConsensusBoundary.CoversLowerEvenMedianAndFutureBoundaries` |
| C21 Work/cumulative selection | consensus/pow.cpp; chain/chainstate.cpp | `PowWork.CalculatesExactWorkWithoutFloatingPoint` | `ChainStateSelection.RejectsInvalidCompetingBlocksWithoutChangingActiveState` | `ChainWork.AccumulatesExactRegtestPowLimitWork` |
| C22 Equal-work tie and Uint512 overflow | chain/chainstate.cpp | `MainnetConsensusBoundary.EqualWorkTieDoesNotDependOnArrivalOrder` | `MainnetConsensusBoundary.ChainworkOverflowLeavesTheAccumulatorUnchanged` | `MainnetConsensusBoundary.ChainworkOverflowLeavesTheAccumulatorUnchanged` |
| C23 Atomic rollback/deep reorg | chain/chainstate.cpp; chain/utxo.cpp | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` | `ChainStateSelection.RejectsInvalidCompetingBlocksWithoutChangingActiveState` | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` |
| C24 Exact network/genesis commitment | consensus/network_params.cpp | `NetworkParams.CommitsExactTestnetGenesisSerializationEvidence` | `NetworkParams.RejectsAnyTestnetGenesisMutation` | `NetworkParams.RejectsAlteredEnabledNetworkParameters` |
| C25 Immutable-v0 version policy | primitives/block.cpp; primitives/transaction.cpp; net/p2p.cpp | `VersionPolicy.SerializedVersionsDoNotSelectConsensusRules` | NONE: no alternate rule exists to activate | `VersionPolicy.HeaderVersionsDoNotSelectConsensusRules`; `VersionPolicy.TransportVersionsAboveTheMinimumDoNotSelectConsensusRules` |

## Stable clause-to-triad trace

This cross-reference maps the current normative clause ledger to the named
evidence inventory. `TRIAD-NAMED` means the table names accepted, rejected, and
boundary evidence; it does **not** establish that the assertion bodies are
exhaustive. `PARTIAL` means that at least one of those cases is still combined,
implicit, or not named. `PROCESS`, `GATE5`, `GATE6`, and `GATE8` are deliberately
outside a unit-test-only Gate 2 claim.

| Ledger clause | Current trace | Gate 2 status |
| --- | --- | --- |
| NCV0-001 | `ExplorerIndexTests.*`, `ExplorerHttp*` | PARTIAL boundary evidence |
| NCV0-002 | `ExplorerIndexTests.ReadsCopiedSnapshotFromNovacoindWithoutMutation` | PARTIAL boundary evidence |
| NCV0-003 | `BlockValidation.*` | PARTIAL route-equivalence evidence |
| NCV0-004 | `Monetary*`, `PowWork.*` | PARTIAL; no-floating-point policy is separately checked |
| NCV0-005 | C01/C02 `Serialization*` | TRIAD-NAMED |
| NCV0-006 | dependency/audit records | PROCESS |
| NCV0-007 | `NetworkParams.*` | PARTIAL parameter-binding evidence |
| NCV0-008 | `MAINNET_CHECKLIST.md` | PROCESS |
| NCV0-009 | `MainnetLock.*`, `NetworkParams.*` | activation-lock evidence, not Gate 2 completion |
| NCV0-010 | `NetworkSelection.*` | activation policy, not Gate 2 completion |
| NCV0-011 | C04 `TransactionStructure.*` | TRIAD-NAMED |
| NCV0-012 | C08 `BlockStructure.*`, `MainnetConsensusBoundary.*` | TRIAD-NAMED |
| NCV0-013 | C05 `BlockValidation.AllowsOrderedRegularDependenciesAndRejectsForwardReferences` | PARTIAL combined vector |
| NCV0-014 | C05 `BlockValidation.*Duplicate*`, `UTXOApply.*` | PARTIAL subcase trace |
| NCV0-015 | C07 maturity vectors | TRIAD-NAMED |
| NCV0-016 | C06 `MainnetMoneyConservation.*` | PARTIAL combined vector |
| NCV0-017 | C15 P2PKH/signature vectors | PARTIAL template subcase trace |
| NCV0-018 | C16 locktime/sequence vectors | TRIAD-NAMED |
| NCV0-019 | C13 block/parent vectors | PARTIAL coinbase-bounds trace |
| NCV0-020 | C12 merkle vectors | TRIAD-NAMED |
| NCV0-021 | C11 coinbase-reward vectors | TRIAD-NAMED |
| NCV0-022 | C14/C15 crypto and signature-hash vectors | PARTIAL script/crypto subcase trace |
| NCV0-023 | C19 `ChainStateDifficulty.*` | PARTIAL adapter-isolation evidence |
| NCV0-024 | C19 `NetworkParams.*`; `NetworkParams.RejectsZeroAndInconsistentDifficultyTimingParameters` | TRIAD-NAMED timing preconditions |
| NCV0-025 | C20 MTP/future-time vectors | TRIAD-NAMED |
| NCV0-026 | `ChainStateSecurity.RejectsFarFutureHeadersWithoutChangingTheActiveTip` | PARTIAL local-time overflow trace |
| NCV0-027 | `MainnetReadiness.Reorganizations*` | GATE5 implementation-dependent |
| NCV0-028 | `BlockJournalTest.*`, `InitializationJournalTest.*` | GATE6 implementation-dependent |
| NCV0-029 | `ChainStateSelection.*`, `Explorer*` | PARTIAL inactive-observer trace |
| NCV0-030 | `SerializationMalformed.*`, `P2P*` | PARTIAL; parser inventory and fuzz remain Gate 7/8 |
| NCV0-031 | `BlockValidation.*WithoutMutation` | PARTIAL rejection-family matrix |
| NCV0-032 | `NetworkInitialization.*`, `SyncProgress.*` | PARTIAL transport-route evidence |
| NCV0-033 | `Wallet*` | PARTIAL non-consensus boundary evidence |
| NCV0-034 | `RpcIntegration.*` | PARTIAL non-consensus boundary evidence |
| NCV0-035 | this matrix | INCOMPLETE until every applicable row has a reviewed triad |
| NCV0-036 | `fuzz/` evidence | GATE8 |
| NCV0-037 | `NetworkSelection.*`, `MainnetLock.*` | activation policy, not Gate 2 completion |
| NCV0-038 | `MainnetLock.*` | activation-lock evidence, not Gate 2 completion |
| NCV0-039 | C25 `VersionPolicy.*` | PARTIAL property-negative trace; no alternate rule is invented |
| NCV0-040 | owner and external-review records | PROCESS |

Broad stand-ins and NONE cells MUST NOT be called exhaustive rule coverage.
Existing retarget/script/time tests elsewhere must be mapped by actual assertions
before claiming missing coverage is closed. Reference checks verify named tests
and implementation-file existence, not assertion adequacy or rule completeness.
The new deep reorg fixture verifies exact selected coins/count after every block
and invariant state on invalid extension. It is coinbase-only; existing signed
spend/undo tests remain mandatory, and deep signed dependency graphs need more
coverage before an exhaustive safety assertion. No consensus behavior changed.

## Scoped P2PKH corpus evidence (not Gate 2 completion)

P2PKH corpus follow-up on `33a448d25308161692ed1339689463a6a4e148b3`:
`ProtocolConformance.SignatureAuthorizationCorpus` replays six manifest-bound
`signature_auth_*.ncpv` records: acceptance, wrong selector, key-hash mismatch,
changed signed output, and signature-push lengths 9/73. Each validation checks
the controlled UTXO view is unchanged. `SignatureAuthorizationCorpusBindsReviewedDigest`
binds the accepted digest and unchanged-signature/different-digest rejection;
`SignatureAuthorizationCorpusPushBoundaries` distinguishes length admission
from strict-DER/low-S and cryptographic validity. See the per-file inventory in
`test/data/protocol/FORMAT.md`. C15, NCV0-017 and NCV0-022 remain PARTIAL;
these records do not claim exhaustive script, DER, key or multi-input coverage.
Gate 1 remains FAIL / NOT FROZEN, Gate 2 incomplete. TESTNET recovery coverage
remains insufficient under the accepted immutable-profile limitation.

## Gate 1 evidence companion (not Gate 2 completion)

`docs/consensus-clause-ledger.md` assigns stable IDs to normative clauses and
rejection families. `nova-freeze-corpus --regtest-testnet` is a diagnostic-only
stdout emitter for canonical compiled REGTEST/TESTNET parameters and the D1–D7
vector identifiers. Its exact-byte, malformed-manifest, repeat-generation, and
working-directory side-effect checks support Gate 1 traceability only. They do
not fill `NONE` cells, establish exhaustive Gate 2 coverage, freeze the
protocol, or authorize a network.

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

## Scoped public signature-fixture additions

Base: `1b83e00f0e49a8703d0a6fef1291ccc538ca732e`. Protocol section references
below identify normative clauses; NCV0 IDs identify existing ledger families,
not claims of complete family coverage. Production behavior is unchanged.

| Protocol / ledger clause | Valid control | Rejection case | Boundary evidence / limitation |
| --- | --- | --- | --- |
| §3 compressed key; NCV0-022 | Original valid public-key authorization | `signature_auth_key_prefix.ncpv`, exact 33-byte prefix04 | `OwnerCompressedKeyBoundaries` checks 32/33/34-byte key parsing; complete curve-point corpus remains partial |
| §3 strict DER; NCV0-022 | Original valid signature | `signature_auth_der_tag.ncpv`, sequence tag31 | `OwnerDerScalarAndLowSBoundaries` covers public scalar/low-S boundaries; parser acceptance is not ECDSA validity |
| §8 exactly two pushes; NCV0-017/022 | Original valid authorization | `signature_auth_trailing.ncpv`, one extra byte | `SignatureAuthorizationBindsSingleFaultMutations` checks exact mutation and restored transaction |
| §8 signature push9..73; NCV0-017/022 | Original valid push71 | `signature_auth_push_below.ncpv` and `signature_auth_push_above.ncpv` | `SignatureAuthorizationRejectsAdjacentPushLengths` checks8/74; existing `CorpusPushBoundaries` covers rejecting9/73, not maximum-valid ECDSA |
| §§7–8 supported locking condition; NCV0-017 | Original P2PKH fixture | `signature_auth_unsupported_lock.ncpv`, lock51 | Existing storage-then-spend test distinguishes output admission from spend authorization; zero-valued-output retention remains separately incomplete |
| §§8–9 conservation and fee; NCV0-016 | Original input5,000,000,000/output4,900,000,000, fee100,000,000 | `signature_auth_amount_insufficient.ncpv`, input below output by1 | `signature_auth_amount_equal.ncpv` accepts equality/fee0; `SignatureAuthorizationPreimageAndReferencedAmount` checks exact golden preimage/digest and unchanged transaction/signature |

All fourteen signature records replay in `SignatureAuthorizationCorpus` through
the existing public validator and check a controlled two-coin view for mutation.
The eight additional records preserve every original corpus hash. Amount-only
records are supplied API coin contexts, not independently valid chain histories.
No input-amount field is added to sighash; no script interpreter or selector is
introduced. Full multi-input, maximum-valid signature, production-limit and
failure-model evidence remain incomplete.

Approved integer difficulty calculations still require reviewed complete
ancestry/header fixtures before full adapter replay. Scaling-failure, ancestry,
Synchronizer inheritance and atomicity contradictions remain open. No copied
arithmetic helper or unreviewed profile substitutes for that evidence. Gate 1
remains FAIL / NOT FROZEN; Gate 2 remains incomplete; MAINNET remains locked.
