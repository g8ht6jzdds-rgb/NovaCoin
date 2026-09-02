# NovaCoin TESTNET on-call, incident, monitoring, and rollback runbook

**Artifact ID:** `NOVACOIN-TESTNET-OPS-002`
**Status:** **PENDING — TEMPLATE; NOT AUTHORIZATION TO DEPLOY**

This record is for a future public TESTNET only. It does not enable TESTNET or
MAINNET. Planned systems, unverified accounts, and self-attestations are not
completion evidence.

## On-call, escalation, and authority

| Duty | Assigned person | UTC coverage | Secure incident-contact method | Authority | Status |
| --- | --- | --- | --- | --- | --- |
| Primary on-call | ALI NOUR EL HAJJ | Pending recorded rota | Pending private incident-channel reference | Initial triage and escalation | Assigned; evidence pending |
| Secondary on-call | **Pending — genuinely independent person required** | Pending recorded rota | Pending private incident-channel reference | Takes primary duty; independently confirms SEV-1 action | Blocked |
| Consensus escalation owner | Pending named maintainer | Pending | Pending secure contact reference | Assess consensus split/invalid-chain evidence | Pending |
| Security response owner | Pending independent reviewer | Pending | Pending secure contact reference | Credential/key/host compromise response | Pending |
| Release-pause authority | Pending release owner plus independent approver | Pending | Pending secure contact reference | Pause distribution and revoke publication | Pending |
| Rollback authority | Pending operations owner plus consensus owner for chain-safety incidents | Pending | Pending secure contact reference | Withdraw bootstrap, stop services, execute approved recovery | Pending |
| Public-status owner | Pending named communications owner | Pending | Pending public-status account and approval path | Publish approved factual updates | Pending |

The secondary must be a different person with a separately controlled account
and reachable backup method. A second email for the primary or a shared account
does not satisfy independence.

Escalation order:

1. Record UTC time, alert ID, service aliases, release hash, and observed evidence.
2. Page the secondary for all SEV-1/SEV-2 events and any distribution, bootstrap, or wallet-custody action.
3. Page the consensus owner for a suspected split, invalid acceptance, genesis/network mismatch, or chainwork divergence.
4. Page the security owner for credential, wallet, signing-key, host, or access-control compromise.
5. Authorities approve containment; the public-status owner communicates only reviewed facts.

| Action | Minimum approval | Never do |
| --- | --- | --- |
| Isolated service restart and redacted telemetry collection | Primary on-call | Change consensus parameters or delete chain data |
| Disable faucet payouts | Primary plus faucet custodian | Export wallet keys or expose RPC |
| Remove bootstrap endpoint | Rollback/operations owner plus primary | Alter peer acceptance or chain selection |
| Pause release/distribution | Release authority plus independent approver | Replace/re-sign artifacts without provenance |
| Consensus split containment | Consensus plus security owner | Manually edit UTXO, index, header, timestamp, or target state |
| Journal/wallet restore | Rollback authority plus asset custodian | Restore an unverified backup |

## Incident and public-status channels

The actual incident-channel identifier, invite link, dashboard URL, and pager
route must stay in a protected evidence system, not Git.

| Control | Required evidence | Status |
| --- | --- | --- |
| Private incident channel | Provider/account alias, access-control review date, membership restricted to named owners | Pending |
| Access changes | Audited approval and UTC audit-export reference | Pending |
| Pager route | Tested primary, independent-secondary, security, and consensus notification route | Pending |
| Public-status channel | Named owner, public account/status-site alias, and approval path | Pending |
| Incident timeline | UTC log template, retention duration, and redaction policy | Pending |

## Severity and response objectives

| Severity | Examples | Acknowledge | Escalate | Public-status target | Initial action |
| --- | --- | ---: | ---: | ---: | --- |
| SEV-1 | Consensus split, invalid block acceptance, compromised signing/wallet credential, chain corruption | 15 min | Immediate | 60 min after verification | Preserve evidence; pause distribution/bootstrap publication only as approved |
| SEV-2 | Persistent fork, journal/wallet recovery failure, widespread crash, access-control failure | 30 min | 30 min | 4 h after verification | Isolate release/host; assess rollback |
| SEV-3 | Seed outage, peer flood, rejection spike, single-region loss, faucet abuse | 4 h | 8 h | 24 h or scheduled update | Apply documented non-consensus mitigation |
| SEV-4 | Capacity warning, dashboard gap, documentation/process defect | 2 business days | As needed | Normally none | Track and remediate |

## Monitoring boundary and collection

Monitoring must run in a provider account and host boundary separate from all
seed-node accounts. It has no node shell, wallet, faucet, deployment, release,
or administrator credential. A local collection agent or private authenticated
tunnel reaches loopback RPC only; port `28332` must never be public.

The collector is limited to authenticated local calls to:

- `getnodehealth`
- `getnodemetrics`
- `getblockchaininfo`
- `getmempoolinfo`
- `getpeerinfo`

Before launch, a socket-level ACL test must prove the collector cannot call
wallet, mining, peer-control, transaction, faucet, or administrator methods.
The protected dashboard shows availability, peer count, transport errors,
height, tip, chainwork, divergence, mempool count/bytes, block delay,
validation failures, disk, memory, CPU, descriptors, and journal/wallet health.

## Alert inventory

Each block must become a real monitoring rule with a non-secret notification
route and protected runbook/dashboard references. `Pending` values block
deployment.

```yaml
Signal: node_availability
Threshold: process probe or authenticated local RPC unavailable
Evaluation period: 2 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#node-availability
Owner: Primary on-call
Retention period: Pending reviewed duration
---
Signal: peer_count
Threshold: zero peers for 10 minutes after bootstrap window, or below approved minimum
Evaluation period: 10 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#peer-count
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: transport_errors
Threshold: p2p_transport_errors rises by 100 above approved baseline
Evaluation period: 5 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#transport-errors
Owner: Primary on-call
Retention period: Pending reviewed duration
---
Signal: height
Threshold: no validated height increase for 3 target intervals after hashrate review
Evaluation period: 30 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#chain-progress
Owner: Consensus escalation owner
Retention period: Pending reviewed duration
---
Signal: tip_and_chainwork_divergence
Threshold: best tip or cumulative chainwork differs between two healthy independent nodes
Evaluation period: 5 minutes
Notification target: Pending protected SEV-1 pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#divergence
Owner: Consensus escalation owner
Retention period: Pending reviewed duration
---
Signal: mempool_count_and_bytes
Threshold: count or bytes reaches 90 percent of configured bound, or changes 80 percent
Evaluation period: 5 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#mempool-pressure
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: block_arrival_delay
Threshold: last block arrival is older than 3 target intervals after hashrate review
Evaluation period: 10 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#chain-progress
Owner: Consensus escalation owner
Retention period: Pending reviewed duration
---
Signal: validation_failures
Threshold: validation_failures rises by 25 above approved baseline
Evaluation period: 5 minutes
Notification target: Pending protected security and operations pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#validation-failures
Owner: Security response owner
Retention period: Pending reviewed duration
---
Signal: disk_capacity
Threshold: free persistent storage below 20 percent or 20 GiB, whichever is larger
Evaluation period: 10 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#resource-pressure
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: memory_pressure
Threshold: host memory use above 85 percent or approved OOM-risk signal
Evaluation period: 10 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#resource-pressure
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: cpu_pressure
Threshold: CPU use above 90 percent with P2P or validation degradation
Evaluation period: 15 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#resource-pressure
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: descriptor_pressure
Threshold: open descriptors above 80 percent of reviewed host limit
Evaluation period: 10 minutes
Notification target: Pending protected pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#resource-pressure
Owner: Operations owner
Retention period: Pending reviewed duration
---
Signal: journal_or_wallet_persistence_failure
Threshold: any journal commit/recovery or encrypted-wallet save/load failure
Evaluation period: immediate
Notification target: Pending protected SEV-1 pager route
Runbook URL: docs/testnet-oncall-monitoring-rollback.md#durability
Owner: Security response owner
Retention period: Pending reviewed duration
```

## Rollback exercise record

Before deployment, stage and record release pause, bootstrap withdrawal, seed
shutdown, safe daemon shutdown, journal recovery, encrypted-wallet restore,
restricted-RPC revocation, faucet kill switch, signed advisory, and clean
redeployment. Do not record secrets.

| Field | Required value |
| --- | --- |
| Exercise UTC date | Pending |
| Participants and independent observer | Pending |
| Exact source/release revision | Pending |
| Systems/regions represented | Pending redacted aliases |
| Start/end time and duration | Pending |
| Actions/results | Pending protected exercise-record reference |
| Defects/remediation | Pending |
| Signer/approval reference | Pending |
| Status | Pending — no exercise performed |

## Completion rule

The primary, genuinely independent secondary, security response owner,
consensus owner, release-pause authority, rollback authority, and public-status
owner must sign the completed record for the exact deployment candidate.
Monitoring must be observed from outside seed accounts and the rollback exercise
must pass. Until then, TESTNET remains disabled.
