# NovaCoin UTXO subsystem

**Status:** in-memory deterministic UTXO transition layer.  Networking and
persistent storage are intentionally outside this component.

## Data model

`UTXOKey` is the exact pair `(Hash256 txid, uint32 output_index)`.  `UTXOSet`
maps that key to `Coin`, which contains the unspent `TxOutput`, creating block
height, and coinbase flag.  The map uses a lexicographic key order over raw hash
bytes followed by output index; no host hash randomization affects behavior.

`UTXOView` is the read-only interface (`GetCoin`, `HaveCoin`).  `UTXOSet` adds
direct `AddCoin` and `SpendCoin` primitives plus transaction/block application.
`GetCoin` returns a copy, so callers cannot mutate chain state through a view.

## Atomic batches and undo

`UTXOBatch` starts with an isolated copy of the set.  Each transaction and undo
operation is also performed against a temporary proposed map.  Only `Commit()`
swaps the complete working map into the base set.  A failed transaction, failed
block, abandoned batch, or allocation failure therefore leaves base chain state
unchanged.

Applying a transaction removes each input coin and adds one `Coin` for each
output at `(txid, output_index)`.  Its `TransactionUndo` stores every removed
coin and every created key.  `BlockUndo` is an ordered vector of transaction
undos; disconnect applies them in reverse order, which supports intra-block
spends and chain reorganization.

## Validation boundary

`UTXOSet` and `UTXOBatch` are state-transition primitives. The authoritative
consensus entry point is `chain::ConnectBlock`, documented in
[validation.md](validation.md). It performs header, structure, script,
maturity, amount, fee, subsidy, and coinbase-reward checks before committing a
batch. Direct low-level UTXO mutation methods are not block-validation entry
points and must not be used to admit network or locally mined blocks.
