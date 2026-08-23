# Local operational observability

`novacoind` exports two authenticated, loopback-only JSON-RPC read methods:

- `getnodehealth` returns the selected network, active height, peer and mempool
  counts, plus process-local counters.
- `getnodemetrics` returns only the process-local counters.

The counters are `blocks_accepted`, `blocks_rejected`,
`transactions_accepted`, `transactions_rejected`, `p2p_messages_dispatched`,
`p2p_disconnects`, and `p2p_transport_errors`. They are unsigned 64-bit
values that saturate at `UINT64_MAX`; they do not wrap, persist, or influence
consensus, chain selection, wallet state, or peer policy.

Network selection is explicit (`--regtest`, `--testnet`, or `--mainnet`). The
daemon validates the immutable selected `NetworkParams` and then requires its
`enabled` flag. TESTNET remains compiled but disabled until the documented
approval gate is completed; selecting it exits before any node, wallet,
listener, or persistent state is created. MAINNET remains disabled.

Regtest-only mutation RPC methods and the CPU mining path remain unavailable
on every other network, including any future enabled TESTNET.
