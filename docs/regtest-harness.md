# Deterministic multi-node REGTEST harness

The v0 harness has two layers. Unit integration tests construct four
independent `RegtestNode` runtimes named `alpha`, `bravo`, `charlie`, and
`delta`. The `nova.regtest_rpc_process` integration test launches four real
`novacoind` processes with independent data directories, journals, encrypted
wallets, P2P/RPC ports, and logs, then drives their authenticated loopback RPC
listeners only. It uses the real TCP P2P transport; test scheduling is bounded
polling and does not decide consensus validity.

This is integration coverage for the real chain, UTXO, mempool, wallet, block
validation, PoW, journal replay, encrypted wallet persistence, TCP P2P, and
authenticated HTTP RPC paths. It is not a claim of public-testnet or
production readiness.

`blocks.dat` is a bounded append-only journal: each record is `u32le length ||
canonical block serialization`.  On restart, a runtime rebuilds its chain and
UTXO state by replaying the journal through `ChainState::AcceptBlock`.  A
truncated, oversized, noncanonical, invalid, or wrongly linked record causes
startup failure; no partial replay is accepted.

The process harness covers peer connection, block propagation, wallet transfer
and transaction relay, mining, confirmation, journal/wallet restart recovery,
partitioned competing branches, most-work reorganization, mempool restoration
after the losing block disconnects, malformed raw-transaction rejection, and
an independent `nova-explorer` process. The explorer must authenticate to the
node snapshot RPC endpoint and then report the selected chain height through
its own authenticated read-only HTTP listener; it never receives a mutable
node-state reference.
`generateregtestblock`, `sendtoaddress`, `connectpeer`, and
`disconnectpeers` are regtest-only RPC adapters. They call the node's ordinary
mining, transaction-relay, and TCP transport interfaces; they never inject a
block into chain state or bypass validation.

On Unix, CTest runs it as `nova.regtest_rpc_process` with the freshly built
`novacoind` executable. It requires only Bash and curl. To invoke it directly:

```sh
bash scripts/regtest_rpc_harness.sh build/debug/src/daemon/novacoind \
    build/debug/src/explorer/nova-explorer
```

The legacy `start_regtest.sh` launcher now requires both
`NOVACOIN_WALLET_PASSPHRASE` and `NOVACOIN_RPC_PASSWORD`; neither is written to
arguments or logs. Scripts create only isolated working directories and remove
their temporary process harness directory on completion.
