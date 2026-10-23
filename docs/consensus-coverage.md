# Consensus rule coverage matrix

Evidence inventory, NOT completeness proof. Named tests contain multiple vectors;
review assertions rather than names. Paths are under src/. `NONE` identifies
missing dedicated coverage and blocks Gate 2. No case is skipped to obtain PASS.

| Rule | Implementation location | Positive test | Negative test | Boundary test |
| --- | --- | --- | --- | --- |
| C01 Fixed endian fields | primitives/serialization.cpp | `SerializationScalars.UsesExplicitLittleEndianEncoding` | `SerializationMalformed.RejectsTruncationNoncanonicalLengthsBoundsAndTrailingBytes` | `SerializationScalars.UsesExplicitLittleEndianEncoding` |
| C02 Minimal CompactSize/count/length | primitives/serialization.cpp | `SerializationCompactSize.UsesShortestCanonicalEncoding` | `SerializationMalformed.RejectsTruncationNoncanonicalLengthsBoundsAndTrailingBytes` | `SerializationCompactSize.UsesShortestCanonicalEncoding` |
| C03 Canonical transaction bytes/IDs/trailing data | primitives/transaction.cpp | `TransactionSerialization.HasCanonicalByteLayoutAndStableTxId` | `TransactionParsing.RejectsMalformedAndNoncanonicalEncodings` | `TransactionStructure.RejectsLimitsAndOversizeProperties` |
| C04 Input/output bounds and uniqueness | primitives/transaction.cpp | `TransactionStructure.AcceptsAValidCoinbaseShapedTransaction` | `TransactionStructure.RejectsInvalidIndependentProperties` | `TransactionStructure.RejectsLimitsAndOversizeProperties` |
| C05 UTXO existence/double spend | chain/validation.cpp | `BlockValidation.ConnectsAndDisconnectsOneSignedBlockAtomically` | `BlockValidation.RejectsMissingUtxoBeforeSignatureChecking` | `BlockValidation.RejectsInflationAndCrossTransactionDoubleSpends` |
| C06 Signed value conservation and fees | chain/validation.cpp | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` | `MainnetMoneyConservation.SignedTransactionsCannotCreateOneAtomicUnit` |
| C07 Coinbase maturity | chain/validation.cpp | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` | `BlockValidation.RejectsImmatureCoinbaseAndInsufficientInputs` | `MainnetConsensusBoundary.CoinbaseMaturityAtThresholdAndAdjacentHeights` |
| C08 Coinbase placement/count/size/height | primitives/block.cpp; chain/validation.cpp | `BlockStructure.SerializesParsesAndHashesCanonicalBlocks` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` | `BlockValidation.RejectsTimestampCoinbaseHeightAndNonfinalTransactions` |
| C09 Halving/final atomic era/zero | consensus/monetary.cpp | `MonetarySubsidy.HasExactIntegerValuesAcrossHalvingBoundaries` | `MonetaryParameters.RejectsInvalidIntervalsConstantsAndExcessIssuance` | `MonetarySubsidy.HasExactIntegerValuesAcrossHalvingBoundaries` |
| C10 Theoretical supply and genesis allocation | consensus/monetary.cpp; consensus/network_params.cpp | `MonetaryIssuance.TestnetCreatorAllocationAndScheduledIssuanceFitTheSupplyCap` | `NetworkParams.RejectsGenesisAllocationThatExceedsTheSupplyCap` | `MainnetReadiness.IssuanceChecksRejectOverflowAndOneUnitExcessReward` |
| C11 Reward/fee/amount overflow | consensus/monetary.cpp | `MonetaryCoinbaseReward.PermitsAtMostSubsidyPlusValidatedFees` | `MonetaryCoinbaseReward.RejectsInflationAndMalformedMonetaryInputs` | `MainnetReadiness.IssuanceChecksRejectOverflowAndOneUnitExcessReward` |
| C12 Merkle ordering/odd duplication/duplicate IDs | primitives/block.cpp | `BlockMerkle.DeterministicVectorsForOneTwoThreeAndOddCounts` | `BlockMerkle.ChangesForChangedTransactionAndOrdering` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` |
| C13 Header/block size and parent | primitives/block.cpp; chain/validation.cpp | `BlockStructure.SerializesParsesAndHashesCanonicalBlocks` | `BlockValidation.RejectsPreviousBlockMismatchBeforeStateMutation` | `BlockStructure.RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules` |
| C14 Key/DER/low-S/ECDSA | crypto/keys.cpp; chain/validation.cpp | `CryptoSignatures.DeterministicSignAndVerify` | `CryptoParsing.RejectsMalformedKeysAndSignatures` | `CryptoParsing.RejectsMalformedKeysAndSignatures` |
| C15 P2PKH template/SIGHASH_ALL | chain/validation.cpp | `BlockValidation.ConnectsAndDisconnectsOneSignedBlockAtomically` | `BlockValidation.RejectsInvalidSignatureWithNoStateMutation` | `MainnetConsensusBoundary.RejectsAlternatePushesSelectorsAndTrailingScriptBytes` |
| C16 Locktime/sequence | chain/validation.cpp | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` | `BlockValidation.RejectsTimestampCoinbaseHeightAndNonfinalTransactions` | `MainnetConsensusBoundary.LocktimeHeightAndTimeEqualityAndSequenceOverride` |
| C17 Compact target canonicality/limit | consensus/pow.cpp | `PowCompactTarget.DecodesAndEncodesCanonicalTargets` | `PowCompactTarget.RejectsMalformedAndNonCanonicalEncodings` | `PowCompactTarget.HandlesBoundaryMantissasWithoutOutOfRangeAccess` |
| C18 Inclusive little-endian PoW | consensus/pow.cpp | `PowValidation.UsesAnInclusiveLittleEndianHashComparison` | `BlockValidation.RejectsProofOfWorkFailures` | `PowValidation.UsesAnInclusiveLittleEndianHashComparison` |
| C19 Retarget/clamps/min-difficulty/rounding | chain/chainstate.cpp | `SyncProgress.RetargetWalkSpansValidatedAndPendingMetadata` | `MainnetConsensusBoundary.RetargetLowerClampAndPowLimitUseValidatedAncestors` | `MainnetConsensusBoundary.RetargetLowerClampAndPowLimitUseValidatedAncestors` |
| C20 MTP/future clock | chain/chainstate.cpp; chain/validation.cpp | `ChainStateSelection.ExtendsAStraightChainByCumulativeWork` | `ChainStateSecurity.RejectsFarFutureHeadersWithoutChangingTheActiveTip` | `MainnetConsensusBoundary.TimestampMedianAndFutureLimitEquality` |
| C21 Work/cumulative selection | consensus/pow.cpp; chain/chainstate.cpp | `PowWork.CalculatesExactWorkWithoutFloatingPoint` | `ChainStateSelection.RejectsInvalidCompetingBlocksWithoutChangingActiveState` | `ConsensusProperties.ChainworkIsStrictlyMonotonicForPositiveWork` |
| C22 Equal-work tie and Uint512 overflow | chain/chainstate.cpp | `MainnetConsensusBoundary.EqualWorkTieDoesNotDependOnArrivalOrder` | `MainnetConsensusBoundary.ChainworkOverflowLeavesTheAccumulatorUnchanged` | `MainnetConsensusBoundary.ChainworkOverflowLeavesTheAccumulatorUnchanged` |
| C23 Atomic rollback/deep reorg | chain/chainstate.cpp; chain/utxo.cpp | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` | `ChainStateSelection.RejectsInvalidCompetingBlocksWithoutChangingActiveState` | `MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred` |
| C24 Exact network/genesis commitment | consensus/network_params.cpp | `NetworkParams.CommitsExactTestnetGenesisSerializationEvidence` | `NetworkParams.RejectsAnyTestnetGenesisMutation` | `NetworkParams.RejectsAlteredEnabledNetworkParameters` |
| C25 Upgrade/version activation | NONE: mechanism unapproved (protocol.md sections 20–21) | NONE: mechanism unapproved | NONE | NONE |

Broad stand-ins and NONE cells MUST NOT be called exhaustive rule coverage.
Existing retarget/script/time tests elsewhere must be mapped by actual assertions
before claiming missing coverage is closed. Reference checks verify named tests
and implementation-file existence, not assertion adequacy or rule completeness.
The new deep reorg fixture verifies exact selected coins/count after every block
and invariant state on invalid extension. It is coinbase-only; existing signed
spend/undo tests remain mandatory, and deep signed dependency graphs need more
coverage before an exhaustive safety assertion. No consensus behavior changed.
