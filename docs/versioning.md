# Semantic versions and engineering milestones

`VERSION` is the single authoritative application version. CMake reads its SemVer
core for project tooling and preserves the full SemVer string for all executable,
RPC and build-metadata reporting. vcpkg's project manifest version is dependency
manager bookkeeping, not an application-version authority. No Git tag, CLI option,
network advertisement or environment variable overrides VERSION.

Current development string: **0.1.0-dev.1**. It retains the previous 0.1.0 core;
it does not retrospectively certify a stable milestone or a public network.

| Family | Engineering milestone |
| --- | --- |
| 0.0.x | Foundational components |
| 0.1.x | Single-node blockchain |
| 0.2.x | P2P networking |
| 0.3.x | Synchronization |
| 0.4.x | Wallet |
| 0.5.x | RPC |
| 0.6.x | Explorer/tooling |
| 0.7.x | Public TESTNET |
| 0.8.x | Security hardening |
| 0.9.x | Release candidates |
| 1.0.0 | Future first MAINNET release — NOT CREATED or approved by TASK023 |

Use strict `MAJOR.MINOR.PATCH` with optional prerelease/build identifiers.
Examples: `0.1.0-dev.1`, `0.7.0-testnet`, `0.9.0-rc.1`, `0.9.0-rc.2+builder.1`.
Leading zeroes in numeric core/prerelease identifiers, empty identifiers and
unbounded arbitrary version text are not valid SemVer. Build identifiers do not
establish precedence; use a SemVer-aware comparison, not lexical string ordering.
NovaCoin bounds a full version string to 128 characters. Patch updates are compatible corrections within an accepted milestone. During
0.x, minor milestones may introduce reviewed incompatibility; never infer safety
from the leading zero. Critical fixes requiring a rule change are not normal
patch releases. No release label is a chain activation signal.

An accepted milestone needs exact-revision tests, documented scope and limitations,
human acceptance and the relevant release checklist. Labels alone do not satisfy
TASK022's mandatory gates. In particular public TESTNET and 1.0.0 require their
actual operational/security evidence; development features already present do
not justify skipping milestones or advertising unsupported release targets.

## Compatibility contracts

| Surface | Required process |
| --- | --- |
| Network protocol | Explicit wire/handshake compatibility analysis, malformed/old/new-peer tests and reviewed network-scoped protocol/minimum-peer values. Application SemVer does not automatically change either field. No seed/endpoint/identity changes inferred. |
| Consensus | Protocol change first, new positive/negative/boundary vectors, activation design, backward compatibility/reorg/rollback analysis, authorized TESTNET testing and attributable human approval. Preserve old rule evidence; requalify affected readiness gates. No silent patch rule changes, height defaults or version-bit activation. |
| Wallet | Versioned data/encryption/key-derivation contract, synthetic-key round-trip/restore tests, backups and explicit migration/recovery authorization before incompatible persistence changes. Never silently reset data or expose private keys. Application version is not a migration command. |
| RPC | Document method/field/error/auth changes and test supported clients. Prefer additive fields/methods, preserve restricted-role allowlists; incompatible behavior requires explicit version/deprecation/migration guidance. Do not enlarge remote-wallet or binding authority. |

`getnetworkinfo` is an authenticated read-only administrator RPC: `version` is the
full application SemVer string; `source_revision` identifies the compiled source;
`protocolversion` and `minimumpeerprotocolversion` come from the selected immutable
network table. Those integers are not the application version. Explorer/faucet
credentials retain their previous narrow methods and cannot call this new method.

## Audit binding

Bind releases to full version, commit/tree, reviewed diff base, exact checks,
immutable tag/public signature identity, pinned build inputs, manifests and human
records. Dirty/unknown/archive-declared builds must remain labeled as such.
No current build timestamp by default; a policy-approved SOURCE_DATE_EPOCH is
deterministic input, not evidence of when the build actually ran.

The [0.7.0-testnet sample](release-checklist-0.7.0-testnet.md) is deliberately
incomplete. The current Gate 1 chainwork specification disagreement and other
TASK022 blockers remain open; versioning infrastructure does not resolve them.
