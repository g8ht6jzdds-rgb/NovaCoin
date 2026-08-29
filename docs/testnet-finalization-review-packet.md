# NovaCoin TESTNET finalization review packet

**Status:** Draft review packet — not an approval and not an activation.

This packet applies to the non-activating source change that sets
`TestnetNetworkParams().deployment_final=true` while preserving
`TestnetNetworkParams().enabled=false`. A reviewer MUST fill in the exact
40-hex commit produced by that change; approval of an earlier genesis-only
candidate does not automatically approve this source revision.

TESTNET remains unavailable at runtime throughout this review. MAINNET remains
`deployment_final=false` and `enabled=false`.

## Required review records

Create one plain-text record per reviewer under
`docs/testnet-finalization-evidence/reviews/`. Each record MUST state the
exact finalization-candidate commit, UTC review date, reviewer identity and
role, evidence reviewed, findings and remaining risks, an explicit decision,
and a detached OpenPGP signature. Do not use placeholders in a record marked
`Approved`.

The required records are:

- `pow-serialization-<candidate>.txt`
- `security-<candidate>.txt`
- `operations-<candidate>.txt`
- `release-<candidate>.txt`

Use this common header, replacing every bracketed field before signing:

```text
Artifact: NOVACOIN-TESTNET-FINALIZATION-REVIEW-001
Area: [PoW and serialization | Security | Operations | Release]
Reviewer: [legal name]
Role: [reviewer role]
UTC review date: [YYYY-MM-DDTHH:MM:SSZ]
Finalization candidate commit: [40-hex commit]
Signer fingerprint: [40- or 64-hex OpenPGP fingerprint]
Evidence reviewed:
- [factual, immutable evidence path or URL]
Findings:
- [finding ID, severity, disposition]
Remaining risks:
- [specific residual risk]
Decision: [Approved | Pending | Rejected]
Comments: [substantive conclusion]
```

Sign the final, saved bytes; editing a signed record invalidates it:

```powershell
gpg --armor --local-user <FINGERPRINT> --detach-sign `
  --output <record>.asc <record>
gpg --status-fd 1 --verify <record>.asc <record>
```

`GOODSIG` and `VALIDSIG` must name the record's declared fingerprint. A valid
signature proves key control only; the release owner must independently verify
the reviewer's identity and any claimed independent environment.

## Independent reproduction and attestation records

Before any approval, two different maintainers MUST reproduce the genesis from
fresh detached checkouts of this exact finalization-candidate commit on
separately controlled machines or cloud accounts. Each maintainer creates a
distinct record and detached signature:

- `docs/testnet-genesis-evidence/reproduction-a-<candidate>.txt(.asc)`
- `docs/testnet-genesis-evidence/reproduction-b-<candidate>.txt(.asc)`

Each record MUST include the source revision, UTC reproduction date, full
signer fingerprint, toolchain versions and installer hashes, vcpkg baseline,
exact generator command, nonce, genesis hash, Merkle root, full canonical
block hex, SHA-256d, and an explicit separate-machine/account-control
statement. The expected values are pinned in
`docs/testnet-genesis-review.md` and must match byte-for-byte.

Each reproducer also signs a separate attestation under
`docs/testnet-genesis-evidence/attestations/` using this form:

```text
Artifact: NOVACOIN-TESTNET-FINALIZATION-REPRODUCTION-001
Role: Reproducer [A | B]
Name: [legal name]
UTC reproduction time: [YYYY-MM-DDTHH:MM:SSZ]
Candidate commit: [40-hex commit]
Signing fingerprint: [40- or 64-hex OpenPGP fingerprint]
Machine/account control: [specific statement of separate control]
Evidence record: [path to the matching reproduction record]
Generator command: [exact command]
Statement: I reproduced and verified the referenced genesis from a fresh
detached checkout using the stated pinned toolchain.
Decision: Approved
```

Reproducer B's public key fingerprint MUST be compared through an independent
out-of-band channel and the verifier MUST sign a separate comparison record.
A valid signature alone does not prove human identity or independent control.

## Area-specific minimum evidence

### PoW and serialization

Review exact genesis bytes, canonical little-endian encoding, exact-consumption
rules, compact-target decoding and canonical encoding, target-boundary vectors,
integer-only work calculation, the complete genesis hex/SHA-256d, and a
one-byte genesis-mutation rejection test.

### Security

Review consensus split, inflation, UTXO/WAL atomicity, chainwork and retarget,
timestamp/reorganization, parser bounds and resource exhaustion, P2P flood
handling, RPC authentication, wallet key handling, and current sanitizer/fuzz
and multi-process evidence. Do not characterize NovaCoin as production-safe.

### Operations

Approval requires factual records for three independent seed operators,
separated infrastructure, on-call and incident ownership, monitoring and log
retention, real-host deployment, safe-shutdown/journal recovery and rollback
exercises, faucet custody/kill switch, and read-only explorer access. Planned
controls are not approval evidence.

### Release

Approval requires a signed immutable tag, hosted CI URL and retained artifact
identifiers/checksums, release-signing identities and custodians, revocation
process, signed checksums/provenance, reproducible-build evidence, and a
distribution location. It must confirm MAINNET remains disabled and TESTNET is
still disabled in this finalization candidate.

## Approval rule

The release owner may create or approve a separate activation change only after
all four signed records are valid, factual, and explicitly approve this exact
finalization commit. The activation change must preserve every genesis vector,
set only `enabled=true` for TESTNET, retain MAINNET disabled, and pass the
activation negative tests.
