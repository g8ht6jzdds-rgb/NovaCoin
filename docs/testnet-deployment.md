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
state.

## Bootstrap

`novacoind --connect host:port` remains the manual bootstrap mechanism. The
repository does not ship hard-coded live seeds and does not implement DNS seeds:
there are no approved independent endpoints yet. Before activation, publish a
signed manifest derived from
`contrib/testnet/bootstrap-manifest.example.json` with real operator names,
endpoints, contacts, expiry, removal process, and detached signature. The
example's `.example.invalid` names are deliberate non-routable placeholders.
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
and a separate read-only credential. Its public frontend, if any, belongs behind
a separately reviewed reverse proxy; node RPC must never be published.

## Faucet and releases

No faucet exists in this repository. A future faucet must be a separate service
with an isolated encrypted TESTNET hot wallet, Base58Check TESTNET-address
validation, bounded request body and queue, per-source/per-address/global rate
limits, payout caps, transaction-ID-only logs, monitoring, and a kill switch.

No signed release packages exist. Before release, establish Linux, Windows, and
macOS signing identities, custodians, revocation procedures, reproducible build
environment, checksum manifest, provenance attestation, and distribution
location. The Dockerfile is a deployment template, not reproducible-build or
release-signing evidence.
