# Changelog

Changes are grouped by engineering milestone, not by claimed operational readiness.
No public release history or historical release date is inferred from a Git commit.

## Unreleased — 0.1.0-dev.1

### Added

- Central semantic application version in `VERSION`, shared by executable version
  reporting, authenticated `getnetworkinfo` and public build metadata.
- Release milestones, compatibility/consensus change policy, sample testnet
  checklist, fail-closed check runner, critical-change report, deterministic
  source archive checksums and custodian-invoked detached-signature support.
- Named consensus, network, wallet and reorganization release-check entries.

### Preserved limitations

- MAINNET remains locked, disabled and non-final. TASK022 gates remain incomplete,
  including the Gate 1 work-equation discrepancy; this task does not fix consensus.
- Existing development components do not establish public TESTNET acceptance,
  independent review/reproduction, supported release targets or licensing closure.
- No 0.7.0 testnet release, signed release, final genesis or 1.0.0 release is created.

## Earlier engineering history

Earlier commits/PRs are development evidence, not retrospectively assigned SemVer
releases. See the exact-commit evidence records and `docs/versioning.md` before
selecting a release baseline or adopting a new milestone.
