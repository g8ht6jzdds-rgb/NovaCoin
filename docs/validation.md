# NovaCoin block-validation pipeline

**Status:** authoritative in-memory connect/disconnect pipeline. The same
`ConnectBlock` entry point is used for every parsed candidate, regardless of
whether it was mined locally or received from a peer. It never reads network,
wallet, storage, or wall-clock state; the caller supplies immutable parameters
and a deterministic parent context.

## Inputs and results

`BlockValidationParams` contains block limits, PoW parameters, monetary
parameters, coinbase maturity, lock-time parameters, the v0 sighash selector,
and the future-time bound. `BlockValidationContext` contains the known parent
block ID, candidate height, median-time-past, explicit validation time, and
already-computed expected compact target.

All validation functions return `BlockValidationError`, not a bare boolean.
`ConnectBlock` returns a `BlockValidationResult` containing a `ConnectedBlock`
only on success. That object contains the UTXO undo data required by
`DisconnectBlock`.

## Validation order

The pipeline fails closed at the first error in this exact order:

1. Validate parameter consistency, the parent relation, expected `bits`,
   canonical target, header hash, PoW, and median/future time bounds.
2. Validate block structural limits: canonical transaction structures, one
   first coinbase, size/count bounds, duplicate transaction IDs, and merkle
   root.
3. Validate the coinbase's minimally encoded height commitment.
4. For every non-coinbase transaction in block order, validate finality,
   duplicate block spends, UTXO existence, coinbase maturity, input/output
   amounts, fee arithmetic, and P2PKH signature authorization.
5. Sum checked fees, validate that the coinbase is no greater than subsidy plus
   fees, apply it to the isolated UTXO batch, then commit the whole batch.

The UTXO batch is a bounded delta (created coins and spent keys) until the
final commit; it never copies the entire base UTXO map. Every failure leaves
the base UTXO state unchanged. `DisconnectBlock` applies the committed block's
undo data in reverse transaction order.

## v0 authorization

The supported locking script is only the exact 25-byte P2PKH form:

```text
0x76 0xa9 0x14 <20-byte HASH160(pubkey)> 0x88 0xac
```

The unlocking script must use the canonical two-push template defined in
`protocol.md`. The validator checks strict DER/low-S parsing, a compressed
public key, `HASH160(pubkey)`, and ECDSA verification against the canonical
`SIGHASH_ALL` preimage. Any other script or encoding is rejected.

The stateless validation layer checks canonical targets and PoW. `ChainState`
derives the expected compact target internally from immutable network
parameters and validated header history before it calls this layer; callers
cannot supply an expected target to block acceptance.
