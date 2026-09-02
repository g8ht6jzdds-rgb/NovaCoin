# NovaCoin TESTNET faucet

**Status: implementation component only. No public faucet is deployed and
TESTNET remains disabled.**

`nova_faucet` is separate from consensus and `novacoind`. It accepts a bounded
request source identifier, a Base58Check address, an integer amount, and a
trusted local request timestamp. It validates the address using only the
immutable TESTNET prefix and checksum before it constructs a `wallet::Recipient`.
REGTEST and MAINNET addresses are rejected. It has no chain-state reference.

The service enforces a fixed configured payout, maximum payout, per-source
window limit, bounded source table, and an in-memory kill switch. Its durable
audit log records timestamp, outcome, SHA-256 source correlation digest, public
address, amount, and transaction ID. It never records a private key or raw
source identity. Audit persistence failure makes the request fail closed.

`FaucetHttpService` and `FaucetLoopbackHttpServer` provide the separate,
bounded local request transport. The listener accepts exactly one bounded
HTTP/1.1 `POST /api/v1/request` per connection, binds only to `127.0.0.1` or
`::1`, rejects chunked and malformed framing, and derives the rate-limit source
identifier from the accepted TCP peer—not from client JSON. The accepted JSON
grammar is deliberately narrow and unambiguous:

```json
{"address":"<TESTNET Base58Check address>","amount":<integer>}
```

It rejects duplicate/reordered fields, floating-point amounts, unknown routes,
oversized bodies, and REGTEST/MAINNET/malformed addresses before the payout
callback is invoked. This loopback listener is intended to sit behind a
separately reviewed public ingress if one is ever approved; it must not be
bound directly to a public interface.

The intended payout implementation is the restricted `faucet` RPC role. That role may call
only `faucetpay`, which is TESTNET-only and rejects an amount above
`NOVACOIN_FAUCET_MAX_PAYOUT`; it cannot call wallet, mining, peer-control, or
explorer methods. The actual wallet stays in a separately operated TESTNET node
and uses the existing authenticated encrypted `wallet.dat` persistence.

Before any public deployment, wire the executable to a separately reviewed
restricted RPC client, configure encrypted secret delivery and hot-wallet
custody, add address-level/global limits and queue accounting, alerts, backups
and recovery, and exercise an out-of-band dual-custodian kill-switch procedure.
This component is not a public faucet service or release authorization by
itself.
