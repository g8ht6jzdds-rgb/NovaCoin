# NovaCoin block format

**Status:** canonical block/header primitive specification.  Proof of work,
difficulty retargeting, chain linkage, and timestamp policy are deliberately not
implemented by this layer.

## Byte-for-byte format

All fields are serialized in this order with explicit little-endian integers.
`Hash256` is its 32 raw digest bytes; transaction counts use canonical
`CompactSize`.

```text
BlockHeader = i32 version
            || Hash256 previous_block_id
            || Hash256 merkle_root
            || u32 time
            || u32 bits
            || u32 nonce

Block = BlockHeader
      || CompactSize(transaction_count)
      || Transaction[transaction_count]
```

`BlockHeader` is exactly 80 bytes.  Its block hash is
`hash256(canonical BlockHeader serialization)`.  A block is fully parsed only
when no trailing bytes remain after its last transaction.

## Coinbase and structural rules

`CoinbaseTransaction` is the special transaction representation whose underlying
`Transaction` has exactly one input containing the null outpoint (all-zero txid,
`0xffffffff` index).  The first block transaction must be coinbase; every later
transaction must not be coinbase.  The coinbase script is bounded by explicit
block limits and must contain 2 through `max_coinbase_script_size` bytes.  Block
placement, reward amount, and height encoding are later block-consensus rules.

`BlockLimits` contains all active bounds: transaction limits, maximum block
serialized size, transaction count, and coinbase script size.  No default
network limit exists.

Structural validation rejects an empty/oversized block, a non-coinbase first
transaction, multiple coinbases, invalid transactions, duplicate transaction
IDs, a bad Merkle root, and malformed/noncanonical nested transaction data.

## Merkle root

Start with transaction IDs in block order.  Pair adjacent IDs and hash each
64-byte concatenation with `hash256`.  Duplicate the final ID at every odd-sized
level, then repeat until one root remains.  A one-transaction block commits that
transaction ID itself.  The ordering and count are therefore committed.
