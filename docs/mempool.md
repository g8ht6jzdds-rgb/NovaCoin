# NovaCoin mempool policy

The mempool is a local, non-consensus cache of unconfirmed transactions.  It
does not establish consensus validity, alter block validation, or determine
the active chain.  A transaction in one node's mempool is not a protocol
commitment and peers are not required to retain or relay it.

## Admission

`AcceptToMempool` receives an explicit transaction-validation context, an
immutable block-validation parameter set, the current active UTXO set, and an
explicit received-time value.  It never reads a wall clock.  The transaction
must pass the same state-independent structure checks, finality checks,
coinbase-maturity checks, amount checks, and P2PKH signature authorization as
a regular transaction in `ConnectBlock`.

The pool has no orphan mechanism in v0.  An input absent from both the active
UTXO set and an already admitted mempool parent is rejected.  A transaction
may spend an output created by an existing mempool transaction only after that
parent has passed validation.  Coinbase transactions and conflicting spends
are rejected.  Admission is also subject to explicit non-consensus resource
limits: transaction count, total serialized bytes, and a minimum fee.

Every entry records its canonical transaction ID, transaction, checked fee,
canonical serialized size, truncated atomic-units-per-byte fee rate, supplied
received time, and the unique IDs of its in-pool parents.  Fees are exact
signed 64-bit `Amount` values and are accepted only when in `[min_fee,
max_money]`; size and aggregate-size additions are checked before insertion.

## Revalidation and removal

`RemoveTransaction` removes the named transaction and every in-pool
descendant.  `RemoveForBlock` removes only transactions confirmed by the
block: descendants are retained temporarily because their parent outputs may
now be available from the active UTXO set under the same transaction ID.

After connecting a block, disconnecting a block, or activating a different
chain branch, callers MUST invoke `RevalidateMempool` with the new active UTXO
set and the applicable next-block transaction-validation context.  It rebuilds
a temporary UTXO overlay in dependency order.  Entries that are no longer
valid, conflict, exceed policy limits, or depend on unavailable parents are
dropped.  Revalidation prepares replacement entry and spend indexes before it
publishes them, so an allocation failure leaves the existing pool unchanged.

## Deterministic block-template ordering

`SelectForBlock` is an assembly policy, not a consensus rule.  Eligible
transactions are selected only after their in-pool parents.  At each step the
pool chooses, among eligible entries that fit the caller-provided byte budget,
the transaction with the greatest truncated fee rate, then the greater exact
fee, then the lexicographically smaller raw transaction ID.  No
floating-point arithmetic is used.  A miner may adopt a different policy
without changing block validity.
