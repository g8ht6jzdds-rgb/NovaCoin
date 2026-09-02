# NovaCoin TESTNET deployment guide

**Status: pre-approval template. TESTNET is disabled; this guide must not be
used to launch a public network until the documented enablement gates close.**

## Network and storage separation

Use only the compiled TESTNET identity: magic `0xDAB5BFFB`, P2P port `28333`,
RPC port `28332`, P2PKH prefix `112`, and the committed TESTNET genesis hash.
The data path must be a TESTNET-specific absolute path such as
`/var/lib/novacoin/testnet`; it must never be a REGTEST or MAINNET path, a
shared volume, or a path populated by another network. The daemon validates its
selected immutable network table before opening node, wallet, journal, or log
state. Before the journal can open it creates or verifies `network.identity`:
the exact 45-byte record `NVID || u32_le(1) || network_id || u32_le(magic) ||
genesis_hash`. A malformed, mismatched, or markerless non-empty directory is
refused. Migrate legacy data only by taking a backup and using a fresh,
network-specific directory; never copy a journal/wallet directory between
networks.

## Bootstrap

`novacoind --connect host:port` remains the manual bootstrap mechanism.
`--bootstrap <path>` loads a bounded canonical static file such as
`contrib/testnet/static-bootstrap.conf.example`. It requires an exact network
name, magic, genesis hash, and unique `host:port` seed lines before dialing
anything. The JSON manifest is an operational review template only and is not
consumed by the daemon. There are no hard-coded live seeds or DNS seeds: no
approved independent endpoints exist. Before activation, publish a separately
reviewed and signed static configuration with real operator names, endpoints,
contacts, expiry, and removal process. The example's `.example.invalid` names
are deliberate non-routable placeholders.
Seeds are discovery hints only. They receive no consensus privilege and every
connection still undergoes normal handshake, framing, PoW, and block validation.

## Container and systemd templates

`contrib/testnet/Dockerfile` builds a non-root `novacoin` image. Use an
immutable source revision and record the base-image digest before making a
release. `docker-compose.testnet.yml` publishes only TCP/28333. It does not
publish RPC/28332; the daemon itself accepts RPC only on loopback and requires
HTTP Basic authentication.

The compose file requires absolute host data and log directories, so chain and
encrypted-wallet state survive container replacement. Populate secrets through
an external secret manager or a mode-0600 environment file; never place
credentials in an image, command line, manifest, repository, or log. The
systemd and logrotate templates provide the equivalent host deployment.

The `--p2pbind 0.0.0.0` option is intentionally permitted only for TESTNET.
Loopback is the default. REGTEST and MAINNET refuse wildcard P2P binding; the
existing TESTNET selection gate still refuses all startup until approval.

## Monitoring and security

Use authenticated loopback RPC probes: `getnodehealth`, `getnodemetrics`,
`getblockchaininfo`, `getmempoolinfo`, and `getpeerinfo`. They expose no private
keys or wallet files. The current node tracks bounded P2P connection/message
limits, framing limits, timeouts, parser rejections, and saturating
non-consensus counters. Journal recovery tests are mandatory for every release.

The independent explorer must use authenticated loopback `getexplorersnapshot`
with the dedicated `explorer` RPC role. That role cannot access node, wallet,
or mutation RPC methods. Its public frontend, if any, belongs behind a
separately reviewed reverse proxy; node RPC must never be published.

## Faucet and releases

`nova_faucet` is an un-deployed separate TESTNET-only component with a bounded
per-source rate limiter, amount cap, wrong-network address rejection, durable
audit log, metrics, and kill switch. Its restricted `faucet` RPC credential is
separate from administrator and explorer credentials. See `docs/faucet.md`.
It is not yet a public request transport or approved custody deployment.

No signed release packages exist. Before release, establish Linux, Windows, and
macOS signing identities, custodians, revocation procedures, reproducible build
environment, checksum manifest, provenance attestation, and distribution
location. The Dockerfile is a deployment template, not reproducible-build or
release-signing evidence.
