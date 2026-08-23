# NovaCoin localhost JSON-RPC (v0)

RPC is node application code and is never a consensus authority.  A request
that constructs, signs, relays, or mines an object must still use the ordinary
mempool, proof-of-work, and chain-state interfaces.  RPC success means the
requested application operation completed; it does not make invalid consensus
data valid.

## Endpoint and authentication

The v0 service accepts JSON-RPC 2.0 request/response objects carried in a
bounded HTTP `POST` body.  The default binding is `127.0.0.1`; the only other
permitted binding is IPv6 loopback `::1`.  A configuration that requests any
non-loopback host is rejected before a daemon transport adapter may use it.
RPC requires HTTP Basic authentication over the local endpoint.  Both
configured credential fields must be non-empty and credentials are compared
without logging either value.

HTTP request headers and body sizes are bounded by explicit `RpcLimits` before
allocation.  Only one JSON-RPC object is accepted per HTTP request.  Requests
must specify `"jsonrpc":"2.0"`, a string `method`, and a non-null `id`.
Unknown fields are ignored; malformed JSON, duplicate object keys, unsupported
JSON numbers, malformed HTTP, invalid credentials, and non-POST requests are
rejected.

`novacoind` runs `LoopbackHttpServer` on its configured RPC port. It accepts
only `POST /` or `POST /rpc` over HTTP/1.1, requires exactly one canonical
`Content-Length`, rejects duplicate length/authentication headers and every
`Transfer-Encoding`, bounds concurrent connections, and closes each socket
after one response. Daemon RPC passwords come only from
`NOVACOIN_RPC_PASSWORD`; they are never command-line arguments or logs. The
daemon passes its live `ChainState`, UTXO set, mempool, P2P peer manager, and
wallet to the service. Wallet address generation persists through the node's
encrypted-wallet path before returning success.

## Methods

Read methods: `getblockchaininfo`, `getblock`, `gettransaction`, `getutxo`,
`getmempoolinfo`, `getmempoolentry`, and `getpeerinfo`.

Enabled non-mainnet networks provide `getexplorersnapshot`, solely for the
separate `nova-explorer` process. It returns no wallet, mempool, or
mutable-state handle: the result is a bounded hex encoding of the versioned
active-chain snapshot defined in `docs/explorer.md`. The daemon computes it
only from its selected active chain after normal validation. The explorer must
authenticate to the ordinary loopback RPC listener and validate the copied
snapshot before use. TESTNET remains disabled, so this does not expose a public
service before approval.

Wallet methods: `getnewaddress`, `getbalances`, `listunspent`,
`createtransaction`, `signtransaction`, and `sendwallettransaction`.
Amounts are signed base-unit JSON integers, never decimal values.  Wallet
methods never expose a private key.  `createtransaction` and `signtransaction`
return canonical transaction hex; `sendwallettransaction` passes the wallet
transaction to the configured mempool broadcast adapter.

Regtest-only `mineregtestheader` receives a canonical 80-byte header as hex and
a bounded integer attempt limit.  It calls the ordinary CPU regtest miner and
returns a valid proof-of-work header/hash when found.  It does not bypass
`ChainState::AcceptBlock`, create a coinbase transaction, or activate a chain.

Additional regtest harness methods are deliberately loopback-only and disabled
outside regtest: `generateregtestblock` delegates to `RegtestNode::MineBlock`,
`sendtoaddress` delegates to the wallet's normal build/sign/mempool/relay
pipeline, and `connectpeer`/`disconnectpeers` control only loopback TCP peers.
They exist to exercise real multi-process regtest flows. None accepts raw
blocks, caller-supplied targets, or a direct chain-state mutation.

## Errors

Responses use the JSON-RPC error object `{code, message}`.  Stable v0 codes:

* `-32600`: invalid request or invalid argument
* `-32601`: method not found
* `-32602`: invalid method parameters
* `-32001`: authentication failure
* `-32002`: resource not found
* `-32003`: wallet operation failed
* `-32004`: node/mempool operation failed
* `-32005`: regtest-only operation refused

Messages are diagnostics, not consensus data.  No endpoint returns private-key
material, seed material, or plaintext keystore exports.
