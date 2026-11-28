# Approved consensus clarification trace

Baseline: `0ecaec5ef299859cb76e90060cf17798aba6f711`. Owner decisions
CFD01–CFD15 and CFP01 selected A in the attributable 2026-10-26 record.
Decision index SHA-256:
`28BFEC56D1B580F5D6AE5B01B23F6D69CCF7A852D41D70C5ADE812DF147E0985`.
The PR head and external tested-source manifest bind subsequent implementation.
All references below are named assertions, not a claim they passed on every OS.
Existing evidence and failed attempts remain historical.

This change clarifies approved rules without changing production code. Existing
NCPV1 records remain byte-identical; new boundary vectors are deterministic
in-test inputs in existing suites. No synthetic difficulty profile, new private
scalar, genesis, parameter, activation or MAINNET change is introduced.

For compactness, PC = `ProtocolConformance`, MB = `MainnetConsensusBoundary`.
Those suite names do not imply MAINNET acceptance. Existing synthetic fixtures
remain synthetic; they are not operational TESTNET evidence.

| Decision / implementation | Valid assertion | Invalid assertion | Boundary assertion / remaining gap |
| --- | --- | --- | --- |
| CFD01 `primitives/serialization.cpp`, `transaction.cpp` | PC.OwnerBoundedParsingRejectsTrailingAndTruncatedTransactions: canonical transaction | same test: trailing byte and truncation | SerializationCompactSize.CoversEveryWireWidthAndCallerBoundary; allocation timing itself remains uninstrumented |
| CFD02 `chain/validation.cpp:IsFinal` | MB.OwnerFinalityZeroAndThresholdSwitch: zero/below threshold; MB.LocktimeHeightAndTimeEqualityAndSequenceOverride | same tests: equal/above comparison | threshold−1/threshold/threshold+1 and sequence override; mixed-input finality remains insufficient |
| CFD03 `primitives/transaction.cpp:OutPoint` | PC.OwnerOutpointPredicatesAndExistenceAreSeparate: nonzero hashes and null shape | same test: malformed zero-hash indexes and missing UTXO | indexes 0/0xfffffffe/0xffffffff; BlockValidation.RejectsSameBlockCoinbaseSpendWithoutStateMutation binds separate placement/existence behavior |
| CFD04 `chain/validation.cpp:HasMinimalCoinbaseHeight`, `primitives/block.cpp` | MB.OwnerCoinbaseHeightSuffixAndSizeBounds: suffixes and minimal heights | same test: size 1/101 and wrong height prefix; MB.RejectsNonMinimalCoinbaseHeightAndFutureTimeOverflow | size 2/100, height 0/u32 max; MB.AcceptsMinimalOrdinaryCoinbaseHeightEncodings binds sign-pad transitions |
| CFD05 `crypto/keys.cpp`, `chain/validation.cpp:CheckInputAuthorization` | PC.SignatureAuthorizationCorpus: existing accepted signed fixture; PC.OwnerCompressedKeyBoundaries | PC.OwnerDerScalarAndLowSBoundaries; PC.SignatureAuthorizationReplayPreservesUtxosForEveryResult | S=floor(n/2) and +1, zero/n scalars, DER minimality, key lengths 32/33/34; PC.SignatureAuthorizationCorpusPushBoundaries. DER-format acceptance alone is not a valid signature |
| CFD06 `consensus/pow.cpp` | PC.OwnerCompactEncodingTruncatesWithoutRoundingUp | same test: noncanonical/sign bit; PowCompactTarget.RejectsMalformedAndNonCanonicalEncodings | mantissa sign normalization, low-byte truncation; PowValidation.UsesAnInclusiveLittleEndianHashComparison and PowCompactTarget.HandlesBoundaryMantissasWithoutOutOfRangeAccess |
| CFD07 `chain/chainstate.cpp:ExpectedBits/ScaleTarget` | MB.RetargetLowerClampAndPowLimitUseValidatedAncestors (existing interval-4 synthetic fixture only) | same test: wrong bits; NetworkParams.RejectsZeroAndInconsistentDifficultyTimingParameters | existing lower clamp and cap; immutable TESTNET interval/ancestor examples and other-null-cause saturation proof remain MISSING; no new profile authorized |
| CFD08 `chain/chainstate.cpp`, `node/sync.cpp:ExpectedBits` | existing immutable-network paths only; no discriminating positive recovery assertion claimed | wrong-bits evidence does not prove the selected ancestor | >1200/equal1200 and non-delayed ancestor discrimination remain insufficient under accepted observability limitation |
| CFD09 `chain/chainstate.cpp:ExpectedBits`, `node/sync.cpp:ExpectedBits` | Complete-ancestry recovery selection remains insufficiently discriminated by immutable-profile vectors | `ChainStateDifficulty.MissingTestnetRecoveryAncestryIsUnavailableContext`, `SyncProgress.MissingTestnetRecoveryContextRollsBackPendingBatch`: unavailable context, no alternate target | `ChainStateDifficulty.SelfLinkedTestnetDifficultyAncestryIsUnavailableContext`, `SyncProgress.SelfLinkedTestnetDifficultyAncestryIsUnavailableContext`: missing/self-linked ancestry at bounded heights rejects without active-state mutation; exhaustive history and route-equivalence coverage remains incomplete |
| CFD10 `consensus/monetary.cpp`, `network_params.cpp` | PC.OwnerExactFullSchedulesAndFinalEra: both exact totals | same test: zero interval; MonetaryCoinbaseReward.RejectsInflationAndMalformedMonetaryInputs | final era / zero era and cap; NetworkParams.RejectsGenesisAllocationThatExceedsTheSupplyCap; actual-chain supply not measured |
| CFD11 `chain/chainstate.cpp:AcceptBlock`, `chain/validation.cpp:ConnectBlock` | BlockValidation.ConnectsAndDisconnectsOneSignedBlockAtomically; ChainStateSelection.ExtendsAStraightChainByCumulativeWork | BlockValidation.RejectsProofOfWorkFailures; ChainStateSelection.RejectsInvalidCompetingBlocksWithoutChangingActiveState | MainnetConsensusBoundary.EqualWorkTieDoesNotDependOnArrivalOrder; all-origin route equivalence remains insufficient; no mandatory error priority claimed |
| CFD12 `chain/chainstate.cpp:ReorganizeChain`, `chain/utxo.cpp` | MainnetReadiness.ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred (normal synthetic cases) | UTXOApply.RollsBackMultiTransactionBlockOnFailure | UTXOBatchAtomicity.UsesABoundedOverlayAndRollbackDoesNotTouchTheBaseSet; unconditional failure atomicity remains FAIL: allocation/crash/restore and observable notification tests not supplied |
| CFD13 `consensus/network_params.cpp:GetNetworkParams`, `node/network_selection.cpp`, `chain/network_chainstate.cpp` | NetworkSelection.AdmitsOnlyEnabledAndValidRegtestParameters | NetworkInitialization.RegtestUnchangedAndInvalidConstructionFailsClosed; NetworkSelection.OwnerUnknownNetworkNeverAdmitsFallback | unknown enum values 3/255, locked MAINNET and valid REGTEST/TESTNET controls; not a proof for every caller/config/storage route |
| CFD14 `chain/chainstate.cpp:ExpectedBits`, `node/sync.cpp:ExpectedBits` | `SyncProgress.NoRetargetingInheritsValidatedAndPendingParentBits`: valid inherited harder-anchor headers in validated and pending-parent paths | same test rejects mismatched bits; `ChainStateDifficulty.NoRetargetingInheritsParentExpectedBits` rejects a powLimit fallback for a harder anchor | Sync anchors cover `0x206fffff`, `0x2070fffe`, and powLimit; rejected inputs preserve header/active state. The earlier no-retarget adapter gap is resolved on these paths; this does not close full difficulty triads or alter D2-A |
| CFD15 `chain/validation.cpp:CheckBlockHeader` | MB.OwnerFutureClockExactOverflowThreshold: largest safe clock and max timestamp | same test: first unsafe clock, unchanged context/UTXO | MB.TimestampMedianAndFutureLimitEquality; real even/odd/short-history median construction remains insufficient |
| CFP01 `net/p2p.cpp:MessageParser`, `net/transport.cpp:Pump` (non-consensus) | P2PFraming.OwnerEveryFragmentBoundaryWaitsForCompleteFrame | same test: malformed complete checksum; P2PFraming.RejectsBadChecksumMagicAndDeclaredOversize | all split points including empty prefix and one-byte remainder; transport timeout/close branch coverage remains partial |

## Required follow-up; no automatic closure

CFD07's arithmetic proof, CFD09's exhaustive recovery/history invariants, CFD12's
unconditional atomicity evidence and CFD13's every-caller non-admission proof
are not completed by documenting their requirements. CFD14's no-retarget
adapter alignment is implemented and has the named harder-anchor regression
coverage; broader difficulty coverage remains incomplete. CFD08's immutable
profile observability limitation remains. Existing test vectors must not be
relabeled as proof of unobserved behavior. No assertions are weakened, no
failures replaced by skips, and no policy is inferred from a passing test.

Gate 1 remains **FAIL / NOT FROZEN**, Gate 2 incomplete; external human freeze
review remains required. MAINNET remains locked and undecided MAINNET parameters,
economics and genesis remain **UNFINALIZED**.

## Current clarification evidence bindings

The documentation-only binding batch starts at
`1b83e00f0e49a8703d0a6fef1291ccc538ca732e`; earlier revision references remain
historical. [Machine-readable bindings](consensus-evidence-bindings.json) cover
CFD01–CFD06, CFD10, CFD15 and CFP01 using existing assertion bodies and vector
bytes. Each case identifies its exact protocol token, source/test file hash and
location, role and remaining gap. No new NCPV1 records or result vocabulary
are added; PR44 pure vectors remain separate until merged and validated.

The checker rejects stale references, modified bound bytes and malformed records.
It does not establish assertion adequacy, decoder allocation ordering, complete
triads, independent review or freeze acceptance. CFD07 scaling-failure
classification, CFD09 exhaustive ancestry/recovery coverage, CFD12 atomicity,
and CFD13 caller safety remain open. CFD14's former no-retarget alignment gap
is closed for the tested paths but does not close Gate 2. Gate 1 **FAIL / NOT FROZEN**, Gate 2
incomplete, MAINNET locked.
