# NovaCoin public-testnet operations review

**Artifact ID:** `NOVACOIN-TESTNET-OPS-001`  
**Status:** **PENDING — NOT AUTHORIZATION TO DEPLOY**

This record supplies the operational evidence required by
`NOVACOIN-TESTNET-GENESIS-001`. It does not change consensus rules or enable
TESTNET. Entries must be completed by named release owners from a reviewed,
immutable source revision; placeholders and self-approval are invalid.

The current daemon accepts only `--regtest` and its live peer service selects
`RegtestNetworkParams()` directly. Therefore no public TESTNET node launch is
currently supported. This artifact is a readiness plan and release gate, not a
claim that the missing testnet runtime, monitoring, bootstrap, or release
infrastructure already exists.

## Required operational decisions

| Control | Required approved evidence | Status |
| --- | --- | --- |
| Bootstrap | DNS seed or static bootstrap endpoint ownership, public keys/certificates where used, availability target, and removal procedure | Pending |
| Node release | Versioned binary/source provenance, reproducible-build instructions, checksums/signatures, supported platforms, and rollback package | Pending |
| Monitoring | Named on-call owner, metrics/log retention, alert thresholds, dashboard location, and access-control review | Pending |
| Abuse handling | Connection, message, RPC, and resource limits; incident triage and peer-isolation procedure | Pending |
| Incident response | Severity definitions, contact path, chain-safety escalation process, public communication owner, and disclosure timeline | Pending |
| Rollback | Explicit condition for pausing distribution, revoking bootstrap endpoints, publishing an advisory, and recovering nodes without changing consensus silently | Pending |
| Data/privacy | Log retention and redaction policy; confirmation that RPC/wallet secrets are not collected or published | Pending |
| Legal/name review | Network name, ports, magic, address prefixes, and branding collision review | Pending |
| Test evidence | Linked green CI build, sanitizer/fuzz evidence, real multi-process regtest result, and unresolved-risk sign-off | Pending |
| Runtime readiness | Reviewed implementation and integration test of TESTNET network selection; no REGTEST-only constants in the public daemon path | Pending |

## Required plans

The detailed, non-consensus procedures and acceptance criteria are in
`docs/testnet-public-operations-plan.md`. Its controls are mandatory evidence
for this record; changing documentation alone does not complete them.

## Approval record

| Role | Name | Date (UTC) | Source revision | Signature/reference |
| --- | --- | --- | --- | --- |
| Release owner | Pending | Pending | Pending | Pending |
| Operations owner | Pending | Pending | Pending | Pending |
| Security reviewer | Pending | Pending | Pending | Pending |
| Infrastructure owner | Pending | Pending | Pending | Pending |

## Completion rule

All controls and approval rows must contain real reviewed evidence. The release
owner then links this artifact from `NOVACOIN-TESTNET-GENESIS-001`. A separate
reviewed source commit may only enable TESTNET after the genesis and operations
records are complete. MAINNET remains disabled.
