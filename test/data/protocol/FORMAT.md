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
key-hash mismatch), and `invalid_signature`. No other result defaults to accept.

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
