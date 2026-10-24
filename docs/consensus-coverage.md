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

## Gate 1 evidence companion (not Gate 2 completion)

`docs/consensus-clause-ledger.md` assigns stable IDs to normative clauses and
rejection families. `nova-freeze-corpus --regtest-testnet` is a diagnostic-only
stdout emitter for canonical compiled REGTEST/TESTNET parameters and the D1–D7
vector identifiers. Its exact-byte, malformed-manifest, repeat-generation, and
working-directory side-effect checks support Gate 1 traceability only. They do
not fill `NONE` cells, establish exhaustive Gate 2 coverage, freeze the
protocol, or authorize a network.
