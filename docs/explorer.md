# NovaCoin read-only block explorer

## Scope and safety boundary

`nova_explorer` is a non-consensus, read-only indexing component. It receives
copies of the active chain through `ExplorerSource`. The standalone
`nova-explorer` daemon uses `AuthenticatedRpcSnapshotSource`: it has no node,
chain-state, UTXO, mempool, or mutable-consensus reference. Its only node
input is an authenticated, bounded RPC response containing copied bytes. It
has no API that accepts blocks, modifies UTXOs, changes the mempool, mines,
broadcasts, or calls consensus validation state-transition functions.

The explorer therefore cannot make a block valid, select a chain, or change a
node's state. It is a presentation index over blocks already accepted by
novacoind. A future remote adapter MUST use an authenticated, read-only
snapshot endpoint and retain the same one-way data flow.

## Indexed data and queries

Each rebuild validates that its copied active-chain snapshot is contiguous,
that every supplied block hash and merkle root is correct, and that block and
transaction structural limits are met. It then derives a private UTXO view and
per-transaction fees (`sum(inputs) - sum(outputs)`) with checked `int64`
arithmetic. A malformed snapshot is rejected atomically; the previously
published index remains available.

The index provides current height, best block, current target (the precise
difficulty representation), target spacing, integer estimated blocks/day,
latest blocks, transactions, inputs/outputs, transaction fees, and currently
unspent outputs. It searches blocks by height or hash, transactions by txid,
and UTXOs by recognized address.

NovaCoin uses a network-aware Base58Check P2PKH presentation encoding. The
explorer recognizes an address only when an output script is exactly
`76 a9 14 <20-byte HASH160> 88 ac`; a presentation layer must encode it with
the selected network prefix. Other script forms remain visible as scripts, but
are not assigned an address.

## Service integration

`NovacoindSnapshotSource` remains an in-process test adapter and holds only a
const chain reference. Deployment uses `AuthenticatedRpcSnapshotSource`, which
POSTs only `getexplorersnapshot` to `127.0.0.1` or `::1`, requires HTTP Basic
authentication, caps the complete response at 32 MiB, applies send/receive
timeouts, and strictly decodes the versioned snapshot before rebuilding the
index. Snapshot data is treated as hostile: malformed framing, JSON shape,
hex, lengths, blocks, hashes, or linkage leave the prior index unchanged.

`novacoind` exposes `getexplorersnapshot` on enabled non-mainnet networks. It
serializes the selected active chain as `u32_le version (1)`, compact block
count, then for each block `u32_le height` and a compact-length canonical block
byte vector. The endpoint is read-only and does not expose wallet, mempool, or
UTXO mutation methods through the explorer. TESTNET remains disabled, so this
does not create a public endpoint before the activation gate is satisfied.

Run the independent process with `nova-explorer --regtest|--testnet --rpcport
<node-rpc> --httpport <explorer-http>`. `NOVACOIN_EXPLORER_RPC_PASSWORD`
authenticates its node snapshot request using the dedicated read-only `explorer`
RPC role; `NOVACOIN_EXPLORER_PASSWORD` authenticates its public
loopback frontend under the fixed username `explorer`. Passwords are never
accepted as command-line options or written to logs. The daemon refreshes the
index every two seconds; a failed refresh preserves the last verified index.

## Authenticated HTTP adapter

`ExplorerHttpService` and `ExplorerLoopbackHttpServer` are separate transport
adapters, not consensus or node components. They hold a `const ExplorerIndex`,
accept bounded request metadata, and permit only authenticated `GET` requests.
Basic authentication is checked in constant time after strict Base64 parsing.
The listener binds only to loopback, enforces concurrent-connection, header,
target, response, and idle-time bounds, rejects request bodies and transfer
encodings, and closes each socket after one response.

Available routes are `GET /api/v1/summary`,
`GET /api/v1/blocks/latest?limit=N`,
`GET /api/v1/blocks/height/<height>`,
`GET /api/v1/blocks/hash/<hash>`,
`GET /api/v1/transactions/<txid>`, and
`GET /api/v1/addresses/<base58check-address>/utxos?limit=N`, and
`GET /api/v1/utxos?limit=N`.
All collection routes require a bounded `limit`. All hash text uses the raw canonical
serialized-byte order already accepted by `Hash256::FromHex`; it does not
introduce an address or identifier encoding rule.

The configured immutable network table controls both rendered P2PKH addresses
and address search decoding. A REGTEST or MAINNET Base58Check address is
rejected by a TESTNET explorer rather than silently treated as a Hash160. The
TESTNET summary includes the exact visible notice `NOVA TESTNET — COINS HAVE
NO VALUE`; public reverse-proxy/UI deployment remains an operations gate and
must not proxy node RPC.
