# Local operational observability

`novacoind` exports two authenticated, loopback-only JSON-RPC read methods:

- `getnodehealth` returns the selected network, active height, best block hash,
  active-chain work, peer count, mempool transaction count and serialized size,
  plus process-local counters.
- `getnodemetrics` returns only process-local counters and timestamps.

The counters are `blocks_accepted`, `blocks_rejected`,
`transactions_accepted`, `transactions_rejected`, `p2p_messages_dispatched`,
`p2p_disconnects`, and `p2p_transport_errors`, along with process uptime and
the local wall-clock time at which the most recent accepted block arrived.
`validation_failures` is the saturating sum of rejected blocks and rejected
transactions. Counters are unsigned 64-bit values that saturate at
`UINT64_MAX`; they do not wrap, persist, or influence consensus, chain
selection, wallet state, or peer policy. Chainwork is rendered as fixed-width
hex and is read from the active `BlockIndex`; it is a status export, not an
input to chain selection.

`novacoind` handles `SIGINT` and `SIGTERM` by leaving the event loop, closing
the HTTP and P2P listeners, and atomically persisting its encrypted wallet
before reporting a successful shutdown. A wallet-persistence failure makes the
process exit nonzero. The block journal is committed independently before
active state is published and supplies recovery after interruption.

Network selection is explicit (`--regtest`, `--testnet`, or `--mainnet`). The
daemon validates the immutable selected `NetworkParams` and then requires its
`enabled` flag. TESTNET remains compiled but disabled until the documented
approval gate is completed; selecting it exits before any node, wallet,
listener, or persistent state is created. MAINNET remains disabled.

Regtest-only mutation RPC methods and the CPU mining path remain unavailable
on every other network, including any future enabled TESTNET.
