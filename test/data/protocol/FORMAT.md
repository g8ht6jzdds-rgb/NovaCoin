# NovaCoin protocol vector format

This directory is test data for the normative vector rules in
`docs/protocol.md` section 29.  The source document remains normative if this
file conflicts with it.

Each `.ncpv` file uses UTF-8 without a BOM, ASCII characters only, LF line
endings, and exactly one trailing LF. It starts with `NCPV1`. A record is
canonical `name=value` lines in strictly increasing ASCII name order, followed
by a line containing only `.`. Blank lines, comments, duplicate fields, and
unknown required fields are invalid.

Bytes are lowercase even-length hexadecimal. Decimal values are unsigned
canonical decimal (no sign or leading zeros, except `0`). `MANIFEST.sha256`
is an uppercase SHA-256 manifest over all `.ncpv` files, sorted by relative
path. Test data does not establish a consensus rule that is absent from the
protocol.

## Signature authorization records

`kind=signature_authorization` uses exactly these fields in the order shown:
`expected_result`, `id`, `input_index`, `kind`, `network`,
`referenced_output_script`, `referenced_output_value`, `sighash_selector`,
`source_refs`, `spec_section`, `status`, `transaction`, `unlocking_script`.
All fields are nonempty; unknown or missing fields reject. `network=REGTEST`,
`input_index=0`, `sighash_selector=1`, and `status=TEST_ONLY` bind this corpus.
The transaction's selected unlocking bytes must equal `unlocking_script`.
The replay uses one non-coinbase referenced coin at height zero and validation
context height/median-time `1/1`; it validates without applying a transaction.
Every replay checks that the controlled UTXO view remains unchanged.

Result labels describe existing validation outcomes, not additional consensus
rules: `accept`, `malformed_unlocking_script`, `invalid_public_key` (including
key-hash mismatch), `invalid_signature`, `unsupported_locking_script`, and
`input_value_too_low`. No other result defaults to accept.

| File | Fixed fixture difference | Expected result |
| --- | --- | --- |
| `signature_auth_valid.ncpv` | Reviewed public key, precomputed signature and transaction | `accept` |
| `signature_auth_malformed.ncpv` | Signature's trailing selector changed from `01` to `02`; declared permitted selector remains `1` | `malformed_unlocking_script` |
| `signature_auth_keyhash_mismatch.ncpv` | First byte of referenced key hash changed from `75` to `74`; public key unchanged | `invalid_public_key` |
| `signature_auth_digest_mismatch.ncpv` | Signed output amount reduced from 4,900,000,000 to 4,899,999,999 base units; signature unchanged | `invalid_signature` |
| `signature_auth_boundary.ncpv` | Nine-byte signature push: strict DER `3006020101020101` (public signature components R=1, S=1), then selector `01` | `invalid_signature` |
| `signature_auth_boundary_max.ncpv` | 73-byte signature push: canonical positive 33-byte DER integers R=S=`80` + 30 zero bytes + `01`, then selector `01` | `malformed_unlocking_script` |

Both boundary records retain the 33-byte compressed key and exactly two direct
pushes. The lower boundary is strict-DER/low-S but does not verify this digest.
The upper boundary's S exceeds half the curve order: admitting the push length
does not admit a high-S signature. These are public signature components, not
private signing material. No signature is generated during conformance replay.
The existing exact-field harness does not add a `boundary_case` field; filenames
and IDs distinguish the cases. This is scoped evidence, not exhaustive DER,
key-encoding, script, or multi-input authorization coverage.

## Pure consensus API records

Four additional exact-field kinds use common fields `expected_result`, `id`,
`kind`, `network=REGTEST`, `source_refs`, `spec_section`, `status=TEST_ONLY`:

| Kind | Additional required fields | Expected result |
| --- | --- | --- |
| `sighash_context` | `expected_digest`, `expected_preimage`, `input_index`, `referenced_output_script`, `sighash_selector`, `transaction` | `accept` or `unavailable` |
| `compact_target` | `bits`, `target` | `accept`, `negative`, `zero`, `overflow`, `noncanonical`, `above_pow_limit` |
| `block_work` | `expected_work`, `target` | `accept` or `unavailable` |
| `pow_compare` | `block_hash`, `target` | `accept` or `reject` |

Unknown/missing/empty fields, kinds and result labels fail closed. Unsigned
index/selector decimal conversion is checked before host-width narrowing.
`bits` is exactly eight lowercase hex digits representing numeric u32, not
wire-endian bytes. Targets and work are exactly 32 big-endian numeric bytes;
`block_hash` is 32 raw bytes interpreted little-endian only by the PoW comparison.
Successful digest fields are 32 raw bytes. On an unavailable sighash both expected
preimage/digest are the literal `none`; failed compact decoding or block-work
calculation likewise uses `target=none` or `expected_work=none` respectively.
`none` is an explicit result tag, never a zero/default numeric value.

Sighash context reconstructs transaction scripts, serialization and selector
suffix and checks the exact preimage and the existing ComputeSignatureHash API.
The two-input public fixture is inherited from
`SignatureHash.HasFixedPreimageAndDigestVector`; no signing keys/signature creation
are involved. Its scripts are a digest fixture, not an accepted spend.
Compact cases use the actual REGTEST pow limit. Block-work and PoW comparison
records are pure arithmetic API cases (including non-network target domains and
non-realized header hashes), not admitted network histories.

Eighteen new records cover input-zero/one and invalid input/selector sighashes;
canonical compact target, exact REGTEST limit and five rejection classes;
work at targets one/two/maximum and invalid zero; and inclusive target-1/equality/
target+1 hash comparison. Fixture provenance uses an external Python integer/
wire-layout/hash reference, with existing fixed preimage and unit-vector checks.
These records do not establish difficulty-retarget/recovery, full signature,
UTXO-transition, monetary or reorganization coverage. Gates 1/2 remain incomplete.

## Difficulty-adjustment records

`kind=difficulty_adjustment` uses exactly these 32 fields, encoded in the
canonical sorted order required above:
`candidate_bits`, `candidate_delay_seconds`, `candidate_height`,
`candidate_time`, `clamped_elapsed_seconds`, `expected_bits`,
`expected_elapsed_seconds`, `expected_result`, `genesis_time`, `id`, `kind`,
`network`, `parent_bits`, `parent_delta_seconds`, `parent_height`,
`parent_time`, `pow_limit_bits`, `prefix_blocks`, `prefix_spacing_seconds`,
`represented_target`, `retarget_bits`, `retarget_interval`, `scale_product`,
`scale_quotient`, `scale_remainder`, `scenario`, `source_refs`,
`spec_section`, `status`, `target_spacing_seconds`,
`target_timespan_seconds`, and `truncation_loss`. The only result labels are
`accept` and `unexpected_bits`; the only source binding is
`ChainState.ExpectedBits,ChainState.AcceptBlock`; the section is `15` and the
status is `TEST_ONLY`. Compact values are eight lowercase hexadecimal digits.
The represented target is 64 lowercase hexadecimal digits. Integer values are
canonical decimal strings. Literal `none` is limited to ancestry and scaling
fields that do not apply to the REGTEST no-retargeting cases.

All TESTNET cases use the immutable TESTNET `NetworkParams`; their synthetic
header ancestry is fully admitted by `ChainState` before candidate evaluation.
The corpus binds the 2014-block prefix, its deterministic timestamps, the
retarget parent, and each candidate time. It covers elapsed times 302399,
302400, and 302401 seconds around the lower clamp; the 1200/1201-second
minimum-difficulty threshold after a retarget; and wrong-bits rejections.
The arithmetic fields expose the integer product, quotient, remainder,
represented compact target, and truncation loss from the independent integer
fixture generator. The conformance test replays candidate blocks against the
existing chain-state API and checks rejection non-mutation.

The REGTEST cases use its immutable no-retargeting parameters. They cover one
accepted inherited-bits child and one wrong-bits rejection. Because REGTEST's
parent expected bits equal its pow limit, these cases cannot distinguish
inheritance from an always-pow-limit implementation. TESTNET minimum-difficulty
recovery traversal, upper clamping, quotient saturation, and scaling-failure
classification remain insufficiently evidenced; no test-only network profile
or recovery claim is introduced.

| File | Case | Expected result |
| --- | --- | --- |
| `difficulty_testnet_retarget_302399.ncpv` | Lower-clamp predecessor | `accept` |
| `difficulty_testnet_retarget_302400.ncpv` | Exact lower-clamp boundary | `accept` |
| `difficulty_testnet_retarget_302400_wrong_bits.ncpv` | Retarget expected-bits rejection | `unexpected_bits` |
| `difficulty_testnet_retarget_302401.ncpv` | Just above lower clamp | `accept` |
| `difficulty_testnet_delay_1200.ncpv` | Exact two-spacing delay | `accept` |
| `difficulty_testnet_delay_1200_wrong_bits.ncpv` | Exact threshold wrong-bits rejection | `unexpected_bits` |
| `difficulty_testnet_delay_1201.ncpv` | Strictly delayed minimum-difficulty block | `accept` |
| `difficulty_testnet_delay_1201_wrong_bits.ncpv` | Strict-delay wrong-bits rejection | `unexpected_bits` |
| `difficulty_regtest_no_retarget.ncpv` | REGTEST inherited expected bits | `accept` |
| `difficulty_regtest_no_retarget_wrong_bits.ncpv` | REGTEST wrong-bits rejection | `unexpected_bits` |

These records are a partial calculation corpus, not a difficulty triad for all
documented rules and not Gate 2 closure. TESTNET recovery remains
`IMPLEMENTED BUT INSUFFICIENTLY TESTED` under the accepted observational-limit
decision; no recovery path is exercised here.

## Additional single-fault and amount-context records

These records retain the same thirteen fields, fixed public key and precomputed
signature. No new signature or private scalar is generated. Each script mutation
also updates the transaction's canonical unlocking-length prefix.

| File | Exact change from the valid fixture | Expected result |
| --- | --- | --- |
| `signature_auth_key_prefix.ncpv` | Key remains 33 bytes; prefix `02` becomes `04` | `invalid_public_key` |
| `signature_auth_der_tag.ncpv` | DER sequence tag `30` becomes `31` | `malformed_unlocking_script` |
| `signature_auth_trailing.ncpv` | Append one `00` after the two unlocking pushes | `malformed_unlocking_script` |
| `signature_auth_push_below.ncpv` | Direct push length 8, eight original public payload bytes, then the original key push | `malformed_unlocking_script` |
| `signature_auth_push_above.ncpv` | Direct push length 74, 74 zero bytes, then the original key push | `malformed_unlocking_script` |
| `signature_auth_unsupported_lock.ncpv` | Referenced locking bytes become `51`; transaction/signature unchanged | `unsupported_locking_script` |
| `signature_auth_amount_equal.ncpv` | Referenced amount becomes 4,900,000,000; transaction/signature unchanged | `accept`, fee 0 |
| `signature_auth_amount_insufficient.ncpv` | Referenced amount becomes 4,899,999,999; transaction/signature unchanged | `input_value_too_low` |

The amount-only cases are synthetic UTXO API contexts, not two alternative valid
chain histories. Section 8 signs the transaction and referenced locking bytes,
not a separately supplied prior amount. Both amount cases retain the valid
fixture's digest. Real prior-transaction output changes also change its txid and
therefore its spending outpoint. These records do not imply an amount-commitment
vulnerability or introduce a new signature mode.

Replay now also seeds one unrelated control coin at the same raw transaction hash
and output index 1, amount 1, locking bytes `51`, height 0, non-coinbase. The
selected record uses index 0. Both known entries and total size are compared
before/after validation; no transaction is applied. This proves only the fully
controlled fixture view, not arbitrary block/reorganization atomicity.

The original 9/73 push records remain rejecting boundary controls. Neither is a
maximum-valid signature. Valid DER71/push72 authorization and multi-input public
fixtures remain missing. Difficulty histories and all production remedies remain
outside this corpus addition.
