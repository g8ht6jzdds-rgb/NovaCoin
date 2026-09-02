# NovaCoin TESTNET access-boundary inventory

**Artifact ID:** `NOVACOIN-TESTNET-ACCESS-001`
**Status:** **PENDING — REDACTED TEMPLATE; NOT DEPLOYMENT EVIDENCE**

This is the authoritative redacted inventory for operational accounts, hosts,
and credentials used by a future public TESTNET. It is an operations artifact,
not consensus configuration, a bootstrap record, or authorization to enable
TESTNET. MAINNET remains disabled.

Populate only after the relevant account or host exists. Do not substitute a
planned provider, a personal account, or an unverified verbal assertion for
evidence. Every completed row needs an evidence reference and a detached
signature by the accountable custodian or reviewer.

## Redaction and handling rules

Never place passwords, API tokens, recovery codes, wallet seed phrases,
private keys, wallet files, authentication headers, full host inventories, or
unredacted identity documents in Git, CI artifacts, logs, screenshots, or this
inventory. Use an internal ticket/vault record ID or an access-controlled
evidence location instead. A reference must reveal neither a secret nor a way
to retrieve one without independent authorization.

Each service credential is unique to one role and environment. An individual
administration identity is never reused as a service identity. Shared root,
cloud-owner, wallet-custody, and release-signing accounts are prohibited.

## Required separation matrix

| Role | Separate account/host required | Permitted boundary | Prohibited boundary | Current status |
| --- | --- | --- | --- | --- |
| Seed | Yes: dedicated seed account and public P2P host per operator | Public TESTNET P2P on the approved port; service supervision and encrypted node-data backup | Wallet, faucet, release signing, DNS administration, public RPC, and consensus override | Pending — no operator/host evidence |
| Monitoring | Yes: dedicated read-only monitoring account/host | Read-only collection from approved health/metrics relays and dashboard operation | Node shell, mutable node RPC, wallet, faucet, deployment, and signing access | Pending — no monitoring evidence |
| Explorer | Yes: separate explorer account/host | Authenticated snapshot-only access through the `explorer` RPC role or reviewed relay | Node administrator credential, wallet, mining, peer control, consensus state, faucet custody | Pending — no host/ACL evidence |
| Faucet | Yes: separate faucet account/host | TESTNET-only encrypted hot wallet, restricted faucet RPC, audited payout operation | MAINNET funds, seed/explorer/monitoring credentials, release signing, public node RPC | Pending — no custody/host evidence |
| Administration | Yes: individual MFA-protected administrator identities and audited bastion path | Approved, time-bounded privileged administration for the assigned system | Shared accounts, service-account reuse, wallet custody without a separate ceremony, unaudited direct production access | Pending — no identity/access evidence |

## Account records

Create one record per provider account, service account, host identity, and
privileged individual—not merely one record per role. Add records instead of
overloading a row when a role has multiple operators or hosts. `Pending` is the
only honest value until the referenced evidence is available.

### Seed account / host record

| Field | Redacted required value |
| --- | --- |
| Record ID | `SEED-<region>-<operator-alias>-<sequence>` |
| Provider and account alias | Pending — provider name and non-secret account alias only |
| Host/region boundary | Pending — region and redacted host alias; public hostname/IP is recorded only in the separately signed bootstrap record |
| Primary custodian | Pending — named accountable person and secure contact reference |
| Backup custodian | Pending — different named person and secure contact reference |
| MFA type and recovery process | Pending — e.g. hardware security key plus sealed recovery procedure reference; never recovery codes |
| Permitted actions | Public P2P service operation, node update, backup/restore, and endpoint removal request |
| Prohibited actions | Wallet, faucet, release signing, DNS ownership, public RPC, and consensus parameter changes |
| Credential rotation date | Pending UTC date and rotation ticket/vault reference |
| Revocation process | Pending — disable account/key, remove endpoint from bootstrap, rotate affected credentials, and record incident reference |
| Firewall/security-group review | Pending UTC date; must confirm only approved P2P ingress and no public RPC |
| Evidence reference and signer | Pending — signed operator record and detached-signature path/fingerprint |
| Status | Pending |

### Monitoring account / host record

| Field | Redacted required value |
| --- | --- |
| Record ID | `MON-<provider-alias>-<sequence>` |
| Provider and account alias | Pending |
| Primary / backup custodian | Pending — distinct custodians where practical |
| MFA type and recovery process | Pending — no recovery material in this document |
| Permitted actions | Read-only probe collection, alert routing, dashboard and retention administration |
| Prohibited actions | Node shell, mutable RPC, wallet, faucet, release signing, and deployment control |
| Credential rotation date / revocation process | Pending UTC date and protected evidence reference |
| Firewall/security-group review | Pending UTC date; must confirm monitoring cannot reach public or privileged node interfaces beyond approved collection path |
| Evidence reference and signer | Pending |
| Status | Pending |

### Explorer account / host record

| Field | Redacted required value |
| --- | --- |
| Record ID | `EXP-<provider-alias>-<sequence>` |
| Provider and account alias | Pending |
| Primary / backup custodian | Pending |
| MFA type and recovery process | Pending |
| Permitted actions | Explorer deployment and use of its unique `explorer` snapshot-only credential |
| Prohibited actions | Administrator RPC, wallet, peer control, mining, consensus references, faucet custody, and release signing |
| Credential rotation date / revocation process | Pending UTC date and protected evidence reference; revoke the restricted RPC principal before host retirement |
| Firewall/security-group review | Pending UTC date; must confirm node RPC is not public and explorer path is restricted to the reviewed relay/loopback route |
| Evidence reference and signer | Pending — include ACL/role verification evidence |
| Status | Pending |

### Faucet account / host record

| Field | Redacted required value |
| --- | --- |
| Record ID | `FAUCET-<provider-alias>-<sequence>` |
| Provider and account alias | Pending |
| Primary custodian | Pending — faucet operator; not a seed/explorer service identity |
| Backup/recovery custodian | Pending — separate named custodian and tested encrypted-wallet recovery procedure reference |
| MFA type and recovery process | Pending — hardware-backed MFA preferred; no wallet secret or recovery code here |
| Permitted actions | TESTNET hot-wallet operation, restricted `faucetpay` RPC, audit/metrics, kill switch, approved replenishment procedure |
| Prohibited actions | MAINNET funds, public node RPC, release signing, seed DNS, explorer/monitoring admin, and shared credentials |
| Credential rotation date / revocation process | Pending UTC date and procedure reference; include immediate faucet kill switch and restricted-RPC credential revocation |
| Firewall/security-group review | Pending UTC date; must confirm public request transport is isolated from node RPC and wallet storage |
| Evidence reference and signer | Pending — custody, limit, audit, monitoring, and kill-switch exercise evidence |
| Status | Pending |

### Administration identity record

| Field | Redacted required value |
| --- | --- |
| Record ID | `ADMIN-<person-alias>-<provider-alias>-<sequence>` |
| Provider and account alias | Pending — individual alias only; never shared root credential |
| Primary / backup custodian | Pending — individual owner and separate emergency-access approver |
| MFA type and recovery process | Pending — hardware-backed MFA and dual-control emergency recovery reference |
| Permitted actions | Audited, time-bounded administration through the approved bastion/change process |
| Prohibited actions | Service-account sharing, unaudited direct production access, routine wallet custody, and release signing without the signing ceremony |
| Credential rotation date / revocation process | Pending UTC date and offboarding/incident procedure reference |
| Firewall/security-group review | Pending UTC date for the systems this identity may administer |
| Evidence reference and signer | Pending — access review, audit-log, and signer reference |
| Status | Pending |

## Review and completion

The operations owner verifies role separation and each evidence reference. The
security reviewer verifies MFA/recovery, least privilege, rotation, revocation,
and firewall review evidence. Neither may turn a `Pending` row into `Complete`
without a named signer, UTC date, and independently retrievable redacted
evidence reference. Link the completed signed inventory from
`docs/testnet-operations-review.md` and the deployment report.
