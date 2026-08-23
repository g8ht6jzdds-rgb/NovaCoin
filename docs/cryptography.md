# NovaCoin cryptography

**Status:** implementation specification for the cryptographic foundation.  It
implements the cryptography chosen in [the protocol](protocol.md#3-cryptography-and-identifiers)
and does not activate any network or consensus parameter.

## Dependencies and boundary

`nova_crypto` uses OpenSSL 3's SHA-256 and cryptographically secure random
number generator, and [Bitcoin Core's libsecp256k1](https://github.com/bitcoin-core/secp256k1)
for all secp256k1 and ECDSA operations.  NovaCoin does not implement hash,
elliptic-curve, nonce, or signature arithmetic itself.

The module is a small value-type API.  All parsers return an empty
`std::optional` on malformed input or a cryptographic-library failure; callers
must fail closed.  It emits no logs and provides no stream or textual export for
private keys.

## Hash256

`Hash256` is exactly 32 bytes in SHA-256 digest order.  `Sha256(message)` is
SHA-256 over the supplied bytes.  `DoubleSha256(message)` is SHA-256 of the
32-byte SHA-256 digest, matching protocol `hash256`.  `FromHex` accepts exactly
64 hexadecimal characters only as a developer/test input helper; it is not a
consensus serialization parser.

## Key formats and lifecycle

`PrivateKey` is exactly 32 bytes, big-endian.  It is accepted only if it is a
nonzero scalar strictly below the secp256k1 group order.  Generation calls
OpenSSL `RAND_priv_bytes`, retries an astronomically unlikely invalid scalar,
and fails if the randomness source fails.  The private-key type is move-only;
its owned byte array is cleansed on destruction, reassignment, and move-from.
Applications must treat the explicit `bytes()` export as sensitive secure-storage
input and must never log it.

`PublicKey` is **only** the 33-byte compressed SEC1 encoding: prefix `0x02` or
`0x03`, followed by the 32-byte big-endian x-coordinate.  Parsing requires both
that exact size/prefix and successful libsecp256k1 point validation; serialization
round-trips exactly to the input.  Uncompressed and hybrid public keys are
rejected even if mathematically valid.

`KeyPair` owns a move-only private key and its derived public key.  It can be
generated securely or assembled from an already validated private key.

## Signatures

The signing input is a `Hash256`, never a raw message.  `PrivateKey::Sign`
performs secp256k1 ECDSA using libsecp256k1's default RFC6979 nonce function.
The resulting signature is strict DER `SEQUENCE(INTEGER r, INTEGER s)`, between
8 and 72 bytes, with positive minimally encoded integers, `0 < r,s < n`, and
low-S normalization (`s <= n/2`).  The byte sequence has no trailing data or
alternative encoding.

`Signature::FromDer` rejects wrong sizes, malformed DER, noncanonical DER,
zero/out-of-range components, and high-S signatures.  `PublicKey::Verify`
defensively reparses both its public key and the supplied signature before
calling libsecp256k1; invalid inputs or verification failures return `false`.

## Test evidence

The deterministic tests cover SHA-256 and double-SHA-256 vectors, the
secp256k1 generator public-key vector for private scalar one, deterministic
RFC6979 signing for fixed input, generated key-pair invariants, correct
verification, wrong-key failure, modified-digest failure, malformed private and
public keys, and malformed/noncanonical signatures.  Sanitizer CI runs the same
tests under AddressSanitizer and UndefinedBehaviorSanitizer.
