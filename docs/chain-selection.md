# NovaCoin chain selection

`nova_chain` owns in-memory chain selection.  It is separate from peer
transport, block storage, wallets, and RPC.  `ChainState` is constructed with
an already validated anchor and a UTXO set representing that anchor's state.
The anchor is an explicit bootstrap commitment; this does not yet implement a
network genesis table or persistent block index.

## Indexed metadata

Each accepted block has one `BlockIndex` record containing its raw block ID,
parent ID, height, header time, supplied expected compact target, decoded
target, per-block work, 512-bit cumulative work, validation status, canonical
block object, and the UTXO undo data produced when it is connected.  The
active `Chain` stores the active tip ID and its height without owning index
objects.

`ChainWork` is a 512-bit unsigned integer represented as a fixed 64-byte
big-endian array.  Its checked addition rejects overflow.  Per-block work is
the exact `Target256` work from `CalculateWork`; it is zero-extended before
addition.  No floating-point arithmetic is used.

## Acceptance and selection

`AcceptBlock(block)` first samples its trusted validation clock, computes the
canonical header ID and requires a known valid parent.  It decodes the target,
computes checked cumulative work, derives the expected compact target from the
immutable difficulty parameters and parent header history, constructs the
parent-specific validation context (including deterministic median-time-past),
and calls `CheckBlock`.
The candidate is then connected through the same `ConnectBlock` entry point
used by reorganization.  Thus locally produced and received blocks have no
separate consensus path.

`ActivateBestChain()` chooses the valid indexed tip with greatest cumulative
work.  Equal work is resolved by the lexicographically smaller raw block ID.
This is deterministic and is not a height comparison.

`ReorganizeChain()` is an internal operation.  External callers cannot request
activation of an arbitrary lower-work branch; after an `AcceptBlock` call
returns successfully, the active tip is always the deterministic best valid
tip known to `ChainState`.

No adapter supplies `expected_bits` or validation time. Expected difficulty is
derived internally and validation time comes only from the configured trusted
clock. Reconnection re-derives the target from indexed headers.

## Reorganizations and atomic state

`FindFork(left, right)` aligns heights then walks parent IDs until it finds the
common ancestor.  `ReorganizeChain(new_tip)` builds both paths before mutating
the UTXO set.  It disconnects the old tip down to (but excluding) the fork,
then connects the new branch from the fork outward.  Disconnecting calls
`DisconnectBlock` with the stored undo record, which restores spent coins.

If any disconnect or connect fails, the method disconnects every newly
connected block and reconnects every successfully disconnected old block in
the only order that preserves UTXO dependencies.  It changes the active tip
only after the full new branch has connected.  Failure is reported with a
structured `ChainError`; no partially activated branch is retained.

Blocks that fail preliminary consensus checks are never indexed.  A block
that fails contextual UTXO validation while being connected is removed by
`AcceptBlock` after the old active chain is restored.
