# NovaCoin reference wallet (v0)

The reference wallet is application code, not consensus code.  It may assemble,
sign, locally inspect, and submit a transaction, but neither its local checks
nor a successful broadcast determines validity.  Nodes remain authoritative
only through the ordinary transaction/block consensus pipeline.

## Keys and receiving addresses

`KeyStore` owns generated `KeyPair` values in memory and associates each
compressed secp256k1 public key with its `HASH160` identifier.  The v0 wallet
does not expose a private-key export operation.  It never logs private keys.
The public receiving address representation is deliberately binary rather than
a newly invented text encoding: a 20-byte public-key hash and the matching
canonical P2PKH locking script:

```
76 a9 14 <20-byte HASH160(compressed public key)> 88 ac
```

This is a wallet convention for constructing the current supported script; it
does not add a consensus script type or an address encoding.

## Wallet UTXOs and balances

The wallet tracks only outputs that match one of its receiving scripts.  A
`WalletUTXO` has the UTXO key, output, source height, coinbase marker, and an
explicit confirmed/unconfirmed state.  Confirmed and unconfirmed balances are
reported separately.  Chain scanning and confirmation updates are supplied by
the caller; the wallet does not modify the node UTXO set.

## Deterministic transaction construction

The v0 builder supports P2PKH recipients and spends only confirmed, wallet
owned UTXOs.  It selects inputs deterministically by descending value, then
ascending `(txid, output index)`.  It estimates a conservative fee in base
units per byte with checked integer arithmetic.  The estimate assumes the
maximum canonical DER ECDSA signature (72 bytes), a one-byte sighash marker,
and a 33-byte compressed public key.  The transaction pays that estimate, so
the final signed transaction never pays less than the requested fee rate.
Coinbase outputs are not selected by default because the construction interface
has no chain-height/maturity context.  A regtest-only caller whose explicit
network maturity is zero may opt in through `WalletParams`; a production wallet
must wait for a maturity-aware design.

Change, when positive, is returned to the earliest generated receiving
address.  Inputs are signed with `SIGHASH_ALL` (value `1`) using the same
canonical transaction serialization and signature-hash routine as validation.
The builder verifies every resulting signature locally before returning.  This
is a defense-in-depth check, not a consensus verdict.

## Submission boundary

`TransactionBroadcaster` is an injected application boundary.  `Wallet` calls
it only with a fully built transaction and returns its result unchanged.  A
broadcaster may enqueue, relay, or reject the transaction according to node
policy; final acceptance remains outside the wallet.

## Explicit limitations

There is no persistent encrypted keystore, mnemonic/seed support, plaintext
export, watch-only import, textual address encoding, fee estimation from live
network policy, RBF, or unconfirmed-input spending in v0.  These require
separate reviewed designs and tests before being added.
