# NovaCoin v0 P2P transport

P2P transport is not a consensus authority.  A peer is hostile until every
received byte has passed framing and message parsing.  `nova_net` never calls
`ChainState`, `ConnectBlock`, `AcceptBlock`, or a wallet.  It emits a parsed,
structurally valid `tx` or `block` event to an application handler; that
handler performs consensus validation and chain-state processing.

## Frame

Every message is exactly:

```text
u32le network_magic || command[12] || u32le payload_length || checksum[4] || payload
```

`command` is lower-case ASCII, contains no embedded NUL, and is zero padded to
12 bytes.  The checksum is the first four raw bytes of `hash256(payload)`.
The parser rejects an unexpected magic, non-canonical command, unknown command,
payload above the global limit, command-specific oversized payload, bad
checksum, truncation, and any non-canonical or trailing payload encoding.

All frame and message limits are supplied through explicit `P2PParams`; there
are no defaults.  A parser buffers at most `24 + max_payload` bytes and checks
all declared lengths before allocating or copying payload data.
`minimum_protocol_version` is an explicit parameter; a lower remote version is
malformed for that configured network.

## v0 commands

* `version`: `i32 version || u64 services || i64 timestamp || u64 nonce ||
  string<max_user_agent> user_agent || i32 start_height || u8 relay`.
  `relay` is exactly `0` or `1`.
* `verack`, `getaddr`: empty payloads.
* `ping`, `pong`: one `u64` nonce.
* `addr`: canonical CompactSize array of network addresses.
* `inv`, `getdata`: canonical CompactSize arrays of inventory vectors
  (`u32 type || Hash256`).
* `tx`, `block`: exact canonical transaction or block serialization, bounded
  by the active explicit primitive limits.
* `getheaders`: `i32 version || array<Hash256> locator || Hash256 stop`.
* `headers`: canonical CompactSize array of exactly 80-byte block headers.

A network address is `u64le services || ipv6[16] || u16be port`.  A header is
the canonical 80-byte consensus header serialization; it has no transaction
count in this message.

## Connections and resource controls

`Connection` represents either an inbound or outbound hostile byte stream.
It maintains a bounded outbound frame queue, parser buffer, explicit
handshake/idle deadlines, and a per-peer message-count window.  It sends local
`version` at creation, requires `version` before `verack`, and disconnects on
duplicates, unexpected handshake messages, malformed input, timeout, or any
limit violation.  `PeerManager` enforces the global inbound/outbound connection
limits and exposes parsed events plus queued outbound frames to a transport
adapter.

`TcpTransport` is the runtime adapter for the educational node. It owns native
TCP sockets using RAII, uses nonblocking I/O, limits accepts per polling cycle,
bounds every socket's pending send data, and removes peers on disconnect or
protocol error. It forwards bytes exclusively through `PeerManager::Receive`
and writes only frames returned by `PeerManager::TakeOutbound`.

`PeerService` bridges parsed events to the node layer: headers flow to
`Synchronizer`; full blocks and transactions flow to `RegtestNode`, which uses
the same journaled `ChainState` pipeline as local mining. Sockets never call
consensus state directly.

## Opt-in REGTEST discovery policy

`--peer-discovery` is a node-local, REGTEST-only experiment. It does not enable
TESTNET/MAINNET, change wire messages or grant consensus privileges. Without
the flag, existing manual/static-bootstrap behavior is unchanged. This first
slice admits only canonical IPv4-mapped `127.0.0.1` endpoints with nonzero
ports. Public/private address discovery and native IPv6 gossip are unsupported;
there is no incomplete public-routability classifier or gossip DNS fallback.
Configured manual/DNS peers retain their existing behavior and priority.

Each service owns an in-memory candidate store, never shared across networks.
Wire addresses have no timestamp/network/genesis fields; local admission time
and local network scoping do not authenticate remote genesis identity. Inbound
source ports are never advertised. Only established, numeric outbound targets
are returned, excluding a known requester target and the local listener.
An inbound peer's listening identity is unknown; receivers also reject their
own listening endpoint. These checks do not authenticate peer identity.

The explicit discovery limits are 256 records, 16 per announcing source IP,
64 per destination /16, 16 entries per exchange, one request/reply per source
IP per 60 seconds, and 16 global exchanges per 60 seconds. A source-IP ledger
is bounded and retained across reconnects. Only a solicited first addr response
within ten monotonic seconds is admitted; late/unsolicited/duplicate entries
do not refresh record lifetime or quotas. Candidate lifetime is 1,800 seconds;
at most three attempts are permitted, with 30/60/120-second failure backoff.
Full tables refuse new gossip rather than evicting connected/manual peers.

At most one discovered dial per second, two pending/active discovered peers,
and two total outbound endpoints per destination /16 are allowed, inside the
existing total outbound limit. Capacity is checked before opening a discovery
socket. Numeric-only nonblocking connects have a five-second establishment
deadline; existing protocol handshake limits remain unchanged. Selection uses
OpenSSL randomness, with an injected deterministic source/monotonic clock in
tests; entropy failure causes no discovery dial. Randomness and quotas are not
proof of Sybil/eclipse resistance. No persistence database is added.

This opt-in is for mutually controlled local REGTEST experiments, not
untrusted multi-user hosts or public operations. Allowed loopback candidates
can target local service ports; authenticated RPC remains mandatory and no
discovery message carries credentials. The feature is not a general SSRF or
anti-eclipse security boundary.

Two outbound slots are reserved from discovery for explicit peers. In this
loopback-only slice all announcers share one source IP, so its 16-record quota
is tighter than the generic 256-record ceiling. Requests are made once per
established outbound connection, not periodically on an existing connection.
Failure to allocate a peer snapshot disables discovery rather than treating
unknown live peers as an empty topology and opening duplicate sockets.

Message handlers only update bounded policy state and queue replies. New dials
are deferred until transport dispatch finishes. Disconnect, failed/expired
handshake and clock rollback release pending state without immediately retrying
an endpoint. Blocks/transactions still use the ordinary validator. REGTEST
tests exercise introducer removal and a real direct third-node connection;
passing these tests is not public-network operational readiness.

## Reconnect synchronization progress

The synchronizer merges missing headers from the already validated selected
ChainState when its active tip changes. Reconnect locators describe that selected
body tip, not a stale losing branch or a header-only tip. This is node-local
progress bookkeeping: it neither selects consensus validity nor changes work,
difficulty, genesis or wire rules. Existing header-only branches are retained.

An overlapping header already validated by this synchronizer is idempotent;
its missing body is requested again, at most once per bounded response. Bodies
already in ChainState (including side branches) are not requested again. New
headers retain all existing parent, difficulty, canonical-target, PoW and work-
overflow checks. An invalid suffix or allocation failure rolls back the batch's
new header bookkeeping and best-header update, so retries cannot silently lose
body requests from a partially installed batch. No block bypasses the ordinary
validator/journal path. Existing message/queue limits still apply; this does not
claim durable in-flight requests, a global header-storage bound, multi-page sync
completion, public anti-DoS certification or TESTNET/MAINNET activation.
