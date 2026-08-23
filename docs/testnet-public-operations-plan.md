# NovaCoin public-testnet operations plan

**Status:** **DRAFT — PUBLIC TESTNET NOT READY**

This document defines the minimum operational evidence for a public NovaCoin
testnet. It is not consensus code, a seed list, an authorization, or a
substitute for an incident commander. TESTNET must remain disabled until every
control is implemented, exercised, and approved in
`docs/testnet-operations-review.md`.

## 1. Runtime and bootstrap

### Current blocker

The public daemon is REGTEST-only: it requires `--regtest`, and `PeerService`
uses `RegtestNetworkParams()` directly. Before any public deployment, implement
an explicit, tested `--testnet` selection path that obtains all magic, ports,
PoW, difficulty, monetary, and genesis values from immutable
`TestnetNetworkParams()`. It must reject simultaneous network flags and must
not make TESTNET enabled merely because the command line requests it.

### Bootstrap acceptance criteria

Use signed, versioned static bootstrap records initially. DNS seeds must not be
advertised until a separate resolver/seed implementation, ownership review,
DNSSEC/TLS policy, and removal runbook exist.

Before launch, publish a reviewed bootstrap manifest containing:

- endpoint host/IP, P2P port, operator, region, and service owner;
- the exact TESTNET magic and expected network identity;
- an expiry/review date, health target, and removal/revocation contact;
- a detached release-signing signature and fingerprint; and
- at least two independently operated endpoints in separate failure domains.

Bootstrap peers are discovery hints only. They never bypass version handshake,
message parsing, PoW, block validation, or chainwork selection. A compromised
bootstrap peer must be removable without a consensus or binary update.

## 2. Monitoring and health

No public endpoint launches without a named on-call owner, dashboard URL,
alert route, and access-control review. Monitoring must redact RPC credentials,
wallet passphrases, private keys, full wallet files, and authorization headers.

Required probes and alerts:

| Signal | Collection source | Initial alert condition |
| --- | --- | --- |
| Process availability | local supervisor/process probe | process unavailable for 2 minutes |
| RPC health | authenticated localhost `getblockchaininfo` probe | two consecutive failures |
| P2P reachability | independent TCP handshake probe | no reachable bootstrap node for 10 minutes |
| Chain progress | validated active height/tip from independent nodes | no block for 3 target intervals after hashrate review |
| Fork divergence | active tip and chainwork from at least two nodes | disagreement persists for 5 minutes |
| Resource pressure | host CPU, memory, disk, descriptors, network queues | threshold set below exhaustion and staging-tested |
| Rejection pressure | parser, handshake, PoW, block, transaction rejections | sustained increase above reviewed baseline |
| Journal/wallet durability | replay and encrypted-wallet load probe on staging | any failure is release-blocking |

Retain security-relevant events for a reviewed period and limit access to the
operations and security teams. Do not collect peer payloads or wallet data by
default. A metrics endpoint, structured counters, and an export mechanism are
not yet implemented in the daemon; their implementation and test coverage are
release prerequisites.

## 3. Abuse limits and triage

The following code-level limits must be regression-tested in the TESTNET
runtime before launch. Operators may lower deployment limits only where that
does not change consensus behavior; raising a protocol limit requires review.

| Boundary | Current configured value | Required operational response |
| --- | ---: | --- |
| P2P frame payload | 1,000,000 bytes | disconnect offending peer; preserve parser error counter |
| User agent | 256 bytes | reject malformed `version` payload |
| Address list | 1,000 entries | reject bounded-message violation |
| Inventory / getdata list | 50,000 entries | reject request and investigate sustained source flooding |
| Block-header response | 2,000 headers | reject excessive response |
| Block locator | 101 hashes | reject excessive locator |
| Handshake deadline | 30 seconds | disconnect and count timeout |
| Idle connection deadline | 300 seconds | disconnect idle peer |
| Per-peer message window | 1,000 messages per 60 seconds | disconnect/rate-limit peer |
| Outbound queue | 1,000 frames / 8 MiB | stop queueing and disconnect on limit violation |
| RPC connections | 32 | reject excess loopback client; never expose RPC publicly |
| RPC header/body | 8 KiB / 1 MiB | reject before allocation |

The incident commander must not change consensus parameters, accept malformed
blocks, blacklist an active chain by policy, or alter wallet/UTXO state to
resolve an availability incident.

## 4. Incident and rollback procedures

### Severity and ownership

| Severity | Example | First action | Escalation |
| --- | --- | --- | --- |
| SEV-1 | suspected consensus split, invalid block acceptance, key compromise | pause distribution and bootstrap publication; preserve evidence | consensus, security, release owners |
| SEV-2 | persistent fork, journal corruption, widespread node crash | isolate affected release/endpoint; publish status | operations and consensus leads |
| SEV-3 | bootstrap outage, abuse spike, single-region failure | remove/replace affected endpoint; monitor recovery | operations owner |

All incidents require UTC timestamps, immutable logs, affected binary hashes,
network parameters, and a public status update through the designated channel.
Never publish private keys, wallet data, credentials, or unvalidated peer
claims.

### Rollback

A rollback may pause releases, withdraw bootstrap records, revoke a signed
manifest, or advise operators to stop a binary. It must not silently rewrite
the genesis block, change consensus parameters, delete chain data, or request
wallet-key export.

Before launch, exercise a staging rollback that verifies manifest revocation,
endpoint removal, signed advisory publication, node shutdown, encrypted wallet
backup/restore, journal replay, and clean redeployment. Record the exercise
date, participants, duration, defects, and remediation reference in the
operations review.

## 5. Release signing and distribution

Release artifacts require a signed manifest generated from a clean, immutable
source revision. The manifest must include:

- source commit ID, build instructions, compiler/toolchain identity, and SBOM;
- platform-specific binary and source archive SHA-256 digests;
- canonical TESTNET genesis hash, Merkle root, full block hex, and full-block
  SHA-256d digest;
- signing-key fingerprint, threshold/signature policy, key rotation and
  revocation procedure; and
- test, sanitizer, fuzz, static-analysis, and staging-harness results.

Keep signing keys offline or in an approved hardware-backed service. A release
owner and separate security reviewer must validate signatures and hashes from
independent machines before publication. No unsigned auto-update mechanism is
permitted.

## 6. Security disclosure policy

Publish a dedicated security contact and encrypted-reporting public key before
launch. A report must receive acknowledgement within 72 hours; critical
consensus or key-exposure reports require immediate triage and a coordinated
disclosure timeline agreed with the reporter where feasible.

The policy must define advisory ownership, embargo handling, affected-version
identification, fixed-release signing, operator notification, and a postmortem
deadline. Vulnerability reporters must never be asked to submit private keys,
wallet files, or exploit attempts against public infrastructure.

## Completion evidence

Each section requires a reviewed evidence link, named owner, UTC completion
date, and immutable source/release revision in
`docs/testnet-operations-review.md`. Until then, this plan remains a draft and
TESTNET remains disabled.
