# NovaCoin proof of work

**Status:** implemented primitive rules for target decoding, header-hash
comparison, accumulated-work calculation, and regtest nonce search. Difficulty
retargeting, expected-`bits` selection, timestamps, and chain-work storage are
not implemented by this module.

## Target representation

`Target256` is exactly 32 unsigned bytes in big-endian numeric order. It is an
internal fixed-width integer representation, not a wire object. Its value is
independent of host byte order; no floating-point arithmetic is used.

The header's `bits` field is a little-endian `u32` on the wire. Its numeric
value encodes a Bitcoin-style compact target:

```text
bits = exponent:8 || sign-and-mantissa:24
mantissa = bits & 0x007fffff
```

For an exponent `e` and mantissa `m`, decode the positive integer as:

```text
if e <= 3: target = m >> (8 * (3 - e))
if e >  3: target = m * 256^(e - 3)
```

The sign bit (`bits & 0x00800000`) is never valid. A compact target is rejected
if its mantissa is zero, its sign bit is set, it cannot fit in 256 bits, or
re-encoding its decoded target does not reproduce the exact original `bits`.
This last rule rejects redundant and ambiguous encodings. A decoded target must
also be nonzero and not greater than the selected network's explicit PoW limit.
`TargetFromCompact(bits, parameters)` performs this final network-limit check;
the one-argument decoder is intentionally useful for parameter construction and
test vectors, so callers validating a header must use the parameterized form.

## Proof-of-work comparison

The canonical header hash is `hash256` (double SHA-256) of the canonical
80-byte header encoding. Its raw 32 digest bytes are interpreted as an unsigned
little-endian 256-bit integer only for the PoW comparison. A header passes PoW
exactly when:

```text
u256_little_endian(block_hash) <= decoded_target
```

Equality is valid. This conversion is explicit and does not depend on the
machine's native endianness.

## Work value

For a valid nonzero target `T`, the work credited by this primitive is:

```text
work(T) = floor((2^256 - 1) / (T + 1)) + 1
```

The implementation performs fixed-width bitwise long division. It does not use
floating point, host-dependent integer widths, or a third-party big-number
format.

## Regtest mining

The current development-only regtest PoW limit is deliberately easy:

```text
bits      = 0x207fffff
pow_limit = 0x7fffff0000000000000000000000000000000000000000000000000000000000
```

`MineRegtestBlock` sets that exact `bits` value and tests monotonically
incremented `u32` nonces, beginning at the candidate header's nonce and wrapping
modulo `2^32`. It stops at the caller-provided attempt bound (capped to one
nonce space) and returns no result if no candidate passes. It is intentionally
single-threaded and unoptimized.

This constant is an explicit regtest-only development parameter. It neither
enables TESTNET nor MAINNET, and it does not finalize any non-regtest network
parameter.
