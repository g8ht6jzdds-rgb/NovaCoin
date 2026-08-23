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
