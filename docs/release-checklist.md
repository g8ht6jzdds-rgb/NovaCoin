# Release acceptance checklist

Each item requires evidence at the **exact selected commit**, with owner/reviewer,
commands, exits, artifact references and limitations. Use PASS, FAIL, NOT TESTED
or HUMAN REVIEW REQUIRED; missing evidence is never PASS. This is separate from
the mandatory MAINNET gates and cannot waive them.

## Every engineering release

- [ ] Owner selects version/milestone, prior reviewed baseline and supported targets.
- [ ] VERSION, changelog and compatibility scope reviewed; source clean and bound.
- [ ] Consensus-critical diff report includes deletions, renamed paths and retained designations.
- [ ] Critical matches personally dispositioned; actual consensus changes have
  protocol documentation, new tests, activation design, backward compatibility
  analysis, authorized TESTNET testing and human approval. No silent patch changes.
- [ ] Highest reasonable warnings treated as errors; required clang-tidy completes.
- [ ] Unit, integration and consensus checks pass; missing suites fail preparation.
- [ ] Full CTest and format checks pass. Preserve failures; unavailable checks block.
- [ ] Daemon/CLI/RPC/build metadata report the same application SemVer; source
  revision, compiler, platform, configuration and actual protocol versions recorded.
- [ ] Build timestamp omitted or fixed by accepted reproducibility policy.
- [ ] Pinned target/tool/dependency/license/SBOM and runtime prerequisites accepted.
- [ ] Source archive and complete payload inventory hashed; SHA256SUMS reverified.
- [ ] Custody/public signing fingerprint/revocation policy accepted; custodian
  supplies detached signature and verification, never private-key contents.
- [ ] Independent builders' actual outputs compared where required; disclose shared control.
- [ ] Exact artifact publication permission obtained separately; no automatic publish.

## Additional release-candidate checks

- [ ] ASan and UBSan testing pass on supported instrumentation targets.
- [ ] Network integration (including real multi-process loopback REGTEST) passes.
- [ ] Wallet/RPC/lifecycle integration passes with synthetic keys and recovery controls.
- [ ] Reorg checks retain signed-spend controls and bounded 1/10/100-deep fixtures;
  disclose coinbase-only deep coverage rather than claim exhaustive graphs.
- [ ] Security/static/sanitizer findings have actual dispositions; required failures block.

The canonical `scripts/release.py prepare` runs checks rather than trusting a
hand-written all-PASS file. CTest named subsets are auditable entry points over
existing real assertions; they are not an exhaustive consensus/security claim.
CI's aggregate release-validation job depends on strict/full tests and both
sanitizer jobs and does not publish artifacts or sign anything.

For public TESTNET, independently accepted operators, custody/endpoints,
environment/startup authorization and operational acceptance remain additional
mandatory requirements. A source archive is not a deployed testnet.

For MAINNET, every MAINNET_CHECKLIST gate must PASS with exact human acceptance
and separate activation governance. MAINNET remains locked and 1.0.0 is not
created by this system. See [sample testnet checklist](release-checklist-0.7.0-testnet.md).
