# NovaCoin public-testnet operations review

**Artifact ID:** `NOVACOIN-TESTNET-OPS-001`  
**Status:** **PENDING — NOT AUTHORIZATION TO DEPLOY**

This record supplies the operational evidence required by
`NOVACOIN-TESTNET-GENESIS-001`. It does not change consensus rules or enable
TESTNET. Entries must be completed by named release owners from a reviewed,
immutable source revision; placeholders and self-approval are invalid.

The daemon has a guarded `--testnet` path that selects immutable TESTNET
parameters only after the compiled enablement gate is set. It derives P2P
framing, ports, genesis, difficulty and local storage identity from that table;
it remains unavailable while `enabled=false`. This artifact is a readiness plan
and release gate, not a claim that real seed, monitoring, custody, or release
infrastructure already exists.

## Required operational decisions

| Control | Required approved evidence | Status |
| --- | --- | --- |
| Bootstrap | DNS seed or static bootstrap endpoint ownership, public keys/certificates where used, availability target, and removal procedure | Pending |
| Node release | Versioned binary/source provenance, reproducible-build instructions, checksums/signatures, supported platforms, and rollback package | Pending |
| Monitoring | Named on-call owner, metrics/log retention, alert thresholds, dashboard location, and access-control review | Pending |
| On-call / incident / rollback | Signed primary and genuinely independent secondary rota, private incident channel, authority assignments, alert inventory, and completed rollback exercise | Pending — see `docs/testnet-oncall-monitoring-rollback.md` |
| Abuse handling | Connection, message, RPC, and resource limits; incident triage and peer-isolation procedure | Pending |
| Incident response | Severity definitions, contact path, chain-safety escalation process, public communication owner, and disclosure timeline | Pending |
| Rollback | Explicit condition for pausing distribution, revoking bootstrap endpoints, publishing an advisory, and recovering nodes without changing consensus silently | Pending |
| Data/privacy | Log retention and redaction policy; confirmation that RPC/wallet secrets are not collected or published | Pending |
| Access boundaries | Completed, signed redacted account/host inventory; independent review of MFA, firewall, rotation, and revocation controls | Pending — see `docs/testnet-access-boundary-inventory.md` |
| Legal/name review | Network name, ports, magic, address prefixes, and branding collision review | Pending |
| Test evidence | Linked green CI build, sanitizer/fuzz evidence, real multi-process regtest result, and unresolved-risk sign-off | Pending |
| Runtime readiness | Reviewed implementation and integration test of TESTNET network selection; no REGTEST-only constants in the public daemon path | Pending |
| Staging deployment | Signed REGTEST Docker and systemd host evidence, including non-root execution, persistent data, port checks, shutdown, and recovery | Pending — see `docs/regtest-staging-validation.md` |

## Required plans

The detailed, non-consensus procedures and acceptance criteria are in
`docs/testnet-public-operations-plan.md`. Its controls are mandatory evidence
for this record; changing documentation alone does not complete them.

## Assigned ownership and availability

| Role | Assigned owner | Contact method | UTC availability | Status |
| --- | --- | --- | --- | --- |
| Release owner | ALI NOUR EL HAJJ | Pending secure operations contact | Pending | Assigned; approval pending |
| Operations owner | ALI NOUR EL HAJJ | Pending secure operations contact | Pending | Assigned; approval pending |
| Security reviewer | ALI NOUR EL HAJJ | Pending secure security contact | Pending | Assigned; independent review pending |
| Primary on-call | ALI NOUR EL HAJJ | Pending incident-channel contact | Pending | Assigned; rota pending |
| Secondary on-call | ALI NOUR EL HAJJ | Pending incident-channel contact | Pending | Assigned; backup operator pending |

**Segregation-of-duties risk:** all listed roles are currently assigned to one
person. This is recorded ownership, not a completed approval. Public TESTNET
launch remains blocked until an independent security reviewer and a genuinely
separate secondary on-call/operator are assigned, with real contact methods
and UTC coverage documented.

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
