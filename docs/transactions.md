# NovaCoin transaction format

**Status:** transaction primitive and structural-validation specification.  This
implements the transaction grammar in [protocol section 5](protocol.md#5-transactions-and-utxo-state).
UTXO, scripts, finality, maturity, and fee checks remain a separate future
state-validation layer.

## Amounts and limits

`Amount` is exactly `int64_t` in atomic units.  Currency uses no floating-point
type.  `MAX_MONEY` is not a global constant: it is the explicit `max_money`
field supplied by `TransactionLimits`, which must come from the active network's
immutable consensus parameters.  A transaction output must be in
`[0, max_money]`, and its checked output total must not overflow `int64_t` or
exceed `max_money`.

`TransactionLimits` also supplies the maximum serialized size, input count,
output count, and script byte length.  Missing/invalid limits are not defaulted.

## Byte-for-byte wire grammar

All integer fields are little-endian.  `CompactSize` is the canonical encoding
defined in [serialization.md](serialization.md#compactsize-and-dynamic-fields).

```text
OutPoint = Hash256 transaction_id || u32 output_index

TxInput  = OutPoint previous_output
         || CompactSize(script_sig_length) || script_sig bytes
         || u32 sequence

TxOutput = i64 value
         || CompactSize(script_pubkey_length) || script_pubkey bytes

Transaction = i32 version
            || CompactSize(input_count) || TxInput[input_count]
            || CompactSize(output_count) || TxOutput[output_count]
            || u32 lock_time
```

`Hash256` is written as its 32 digest bytes in serialized order.  `i32 version`
is its two's-complement bit pattern serialized as four bytes; `TxOutput.value`
uses eight bytes.  Scripts are opaque bounded byte vectors at this layer.
There are no native-struct copies, padding bytes, floating-point fields, or
architecture-dependent encodings.

The null outpoint is exactly all-zero `transaction_id` plus
`output_index == 0xffffffff`.  A zero transaction ID with any other index is
malformed and rejected.  A complete null outpoint is structurally permitted as
a coinbase-shaped input; block placement and coinbase-script rules are checked
later with block context.

## TxID, size, and weight

`TxID = hash256(canonical Transaction serialization)`.  Serialization is
refused unless `CheckTransactionStructure` succeeds under explicit limits.
`SerializedSize` uses checked integer arithmetic; with no witness design in v0,
`Weight == SerializedSize` exactly.

## Structural versus state validation

`CheckTransactionStructure` is pure and state-independent.  It checks counts,
limits, malformed/duplicate outpoints, output amounts and total, scripts, and
serialized size.  It never queries UTXOs or executes scripts.

UTXO existence and atomic state transitions are implemented separately in
`nova_chain` through `UTXOSet` and `UTXOBatch`; see [utxo.md](utxo.md).  Script
execution, coinbase maturity, input value/fee, and finality remain future
state-validation rules.  They must call structural validation first and must
never be replaced by wallet logic.
