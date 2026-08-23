# NovaCoin canonical binary serialization

**Status:** implementation specification for consensus-object byte encoding.
This document implements the wire rules in [protocol section 2](protocol.md#2-primitive-notation-and-canonical-wire-format).
It does not define a new transaction, block, or network rule.

## General rules

Consensus objects are serialized field by field in the prescribed order.  Native
C++ structs, host byte order, padding, pointer values, locale, and floating-point
representations are never serialized.  An independently framed object must be
fully consumed by `BinaryReader::RequireEnd()`; otherwise it is invalid due to
trailing bytes.

`BinaryWriter` owns its output bytes.  `BinaryReader` borrows a caller-owned,
bounded byte span and retains no attacker-controlled data after the reader is
destroyed.  Reader failures are sticky and represented by `DeserializationError`.
The first error is retained, so parsing behavior is deterministic.

## Fixed-width scalars

`uint8`, `uint16`, `uint32`, `uint64`, and `int64` are encoded in exactly 1, 2,
4, 8, and 8 bytes respectively.  All use explicit little-endian byte order.
`int64` uses its two's-complement bit pattern, obtained without a numeric cast.
No floating-point type is supported by the serialization API.

`Hash256` is exactly its 32 serialized digest bytes, without a length prefix or
byte reversal.  Fixed byte arrays likewise have no prefix and are copied at their
compile-time size.

## CompactSize and dynamic fields

Byte vectors, strings, and arrays begin with a canonical CompactSize count.

| Value range | Encoding |
| --- | --- |
| `0..252` | one byte |
| `253..65535` | `0xfd` + little-endian `uint16` |
| `65536..4294967295` | `0xfe` + little-endian `uint32` |
| `4294967296..18446744073709551615` | `0xff` + little-endian `uint64` |

The shortest representation is mandatory.  A longer representation of a value
that fits a shorter range yields `kNonCanonicalCompactSize`.

Every caller supplies an explicit maximum for a vector/string byte length or
array element count.  The reader rejects a count over that maximum before
allocation.  It additionally rejects a byte-vector length larger than the bytes
remaining in the frame, and an array whose declared minimum encoded size cannot
fit in the remaining frame, before allocating.  Conversion from a `uint64`
count to `size_t`, aggregate-size arithmetic, and read offsets are checked.

Strings are bounded opaque byte sequences; text encoding validity is not assumed
by this primitive.  A consensus object that requires a particular text encoding
must validate it in that object's own layer.

## API use

`ReadArray` takes the minimum canonical encoded size of an element plus a
deserializer callback.  This makes the caller document the early impossibility
bound for each consensus collection.  Callback exceptions and allocation
failures are converted into reader failures.  Parsing code must check every
`std::optional` result and then call `RequireEnd()` for exact parsing.

Fuzzing is enabled with `-DNOVA_BUILD_FUZZERS=ON` under Clang with libFuzzer
support. Every target is bounded before parsing and links AddressSanitizer plus
UndefinedBehaviorSanitizer. Targets cover generic serialization
(`fuzz_deserializer`), transaction and block decoding, P2P framing,
signature/public-key parsing, standard address-script parsing, and compact
PoW-target decoding. A crash, sanitizer diagnostic, assertion failure, or
non-deterministic acceptance result is a release blocker.
