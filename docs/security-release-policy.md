# Security release and incident policy

**Proposed policy; named human acceptance required before real-value release.**
MAINNET remains locked. No external audit, bounty, response guarantee, independent
review or production safety is claimed merely by publishing this document.

## Intake and triage

Repository owner must designate an actually monitored private reporting channel,
primary/backup responders, coverage and escalation deadlines before Gate 10/14
acceptance. Until established, arrange a private channel with the owner; do not
post exploit details, private keys or user data to public issues. Channel and
coverage status: NOT ESTABLISHED by TASK022; no invented contacts or SLA.

Consensus inflation, invalid-spend acceptance, divergent hashes/state, PoW/work
errors and rollback corruption are release BLOCKERS regardless of severity labels.
Memory unsafety, key leakage, auth bypass, supply-chain compromise and unbounded
remote allocations are blockers pending human disposition. Fail closed; preserve
crashes/reproducers and impacted exact versions. An unavailable check is not PASS.

## Remediation and release

Record the finding, scope, affected inputs/revisions, minimal reproducer, threat
model and independent reviewer. Fix specification first if changing consensus.
Use small reviewed branches with regression tests; never weaken negative tests
or hide historical failures. Re-run relevant sanitizers/static/fuzz/determinism
and independent review. Track coordinated disclosure and a public advisory only
under explicit approved publication scope; report tests honestly.

Custodian must document actual signing storage/access, backups/restore tests,
suspension trigger and delegates, revocation/public announcement and replacement
approval. Public fingerprints alone are not custody evidence. Shared administration
and recovery must be disclosed; nobody may attest another person's independence.
Suspend signing on suspected compromise; use the accepted incident procedure,
not ad hoc key copying. Never include secret contents in audit/release artifacts.

Retain exact artifact/tag/manifests and evidence; do not overwrite historical tags
or silently replace packages. Withdrawal/replacement needs its own approval and
clear public identities. Rollback compatibility/data retention require reviewed
plans; no automatic chain reset, consensus downgrade or network activation.
No key rotation, publication or infrastructure action is performed by this policy.
