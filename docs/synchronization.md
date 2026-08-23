# NovaCoin node synchronization

`nova_node::Synchronizer` sits above P2P transport.  A successful P2P
handshake permits it to send `getheaders` with its verified active tip.  It
does not use a peer's advertised `start_height` for chain selection.

For each `headers` response it requires a known preceding header, the compact
target derived from immutable difficulty parameters and the validated local
header history, a canonical target below the configured PoW limit, valid proof
of work, checked per-header work, and checked cumulative work. It tracks the
greatest verified header chainwork only to determine which missing block IDs to
request through `getdata`.

Header work is not block validity.  On `block`, the synchronizer calls the same
`ChainState::AcceptBlock` pipeline used for local blocks. Validation time is
obtained only from the node's trusted local clock and expected difficulty is
derived internally. Only a successful result can update the
active chain.  Bad headers, bad blocks, unknown parents, and rejected blocks
produce structured errors and no chain mutation.

The synchronizer implements the same no-retarget, testnet min-difficulty, and
retarget boundary rules as `ChainState`; it does not accept a caller-supplied
expected target. The block inventory type and P2P protocol version are likewise
explicit parameters; no network constants are silently chosen.
