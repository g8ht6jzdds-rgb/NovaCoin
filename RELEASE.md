# NovaCoin engineering release procedure

**No publication, mainnet release or service activation is authorized here.**
MAINNET_LOCKED stays true. Read `docs/release-checklist.md`, `docs/versioning.md`,
`docs/release-process.md`, `docs/security-release-policy.md` and MAINNET_CHECKLIST.md.
The last documents retain the independent human/security/operational gates.

## Prepare and review

1. Owner selects the prior reviewed baseline and exact clean candidate, intended
   milestone, supported targets, pinned OS/toolchain/dependencies, complete
   licensing/SBOM inputs, signing identity and separately controlled builders.
   Commit the reviewed VERSION/changelog changes. Do not overwrite old tags.
2. Inspect consensus-sensitive differences, including deletes and both sides of
   a rename (no rename collapsing):

   ```sh
   python3 -B scripts/release.py consensus-diff --base <previous-reviewed-commit> --head HEAD
   ```

   Designations are in `contrib/release/consensus-critical.txt`; the tool retains
   built-in minima and the previous version's designations to prevent silent
   removal. A match highlights review risk, not proof of a consensus rule change.
3. Every consensus-affecting decision requires explicit protocol documentation,
   new consensus tests, activation design, backward-compatibility analysis,
   authorized TESTNET evidence and human approval. Normal patch releases are
   refused for actual consensus rule changes; use a separately reviewed milestone
   and compatibility decision, never hide it in a patch. No version enables rules.
4. Run actual fail-closed preparation on the clean candidate (installed inputs
   only; no automatic dependency installation). Choose a fresh external output
   directory and build root. The runner fixes two workers; configure/compiler,
   required clang-tidy, build, format or test failures stop preparation:

   ```sh
   python3 -B scripts/release.py prepare --base <previous-reviewed-commit> \
     --build-root build/release-validation --output <new-external-directory> \
     --profile development
   ```

   Use `--profile rc` for candidates. A 0.9.x or `-rc.*` version forces the RC
   matrix even if the caller requests development: ASan, UBSan, network integration,
   wallet integration and reorg tests in addition to ordinary gates. Missing tests
   are errors, not skips. Platforms without the Unix process integration harness
   cannot pass this runner's release preparation; support is not inferred.

For designated critical changes the runner additionally requires
`--consensus-decision <reviewed-record.json>`. The record binds `base`, `head`,
the exact sorted `critical_paths`, `decision: ACCEPTED` and attributable `reviewer`.
It must distinguish `effect: NO_CONSENSUS_CHANGE` (with explicit reviewed `rationale`
and a hash-bound `human_approval` reference) from `effect: CONSENSUS_CHANGE`.
No-rule-change classification is a human finding, not an automated semantic proof.
For actual rule changes, each of `protocol_documentation`, `new_consensus_tests`, `activation_design`,
`backward_compatibility_analysis`, `testnet_testing`, `human_approval` must identify
a nonempty local relative `path` and matching `sha256`. Store these public,
non-secret records together outside the repository. File/hash/reference validation
does NOT authenticate a person's identity or judge the substance of approval;
maintainers must verify those facts. Do not manufacture an accepted record or
substitute REGTEST for required operational TESTNET testing.

## Outputs and reproducibility

Successful unsigned preparation emits `CHECKS.json`, `CONSENSUS-CHANGES.json`,
`release-metadata.json`, a Git-commit source TAR and sorted relative `SHA256SUMS`.
Failure retains logs and the change report; it does not create a successful release
record. The archive contains committed files only and uses Git's commit-derived
metadata, not wall-clock timestamps. Known sensitive suffixes/key armor are
rejected before archiving; this is defense in depth, NOT a complete secret detector.
Never commit credentials, wallet data or private release keys in the first place.

Source export pins `core.autocrlf=false`, `core.eol=lf` and `tar.umask=0022`, then
checks every regular TAR member against the commit's blob identity and size,
with canonical permissions `0644` for ordinary files and `0755` for executables,
and requires the complete committed file set. Export attributes that omit or
rewrite content therefore fail preparation. A rejected TAR remains diagnostic
evidence, not a successful release; do not normalize or replace it after hashing.
Historical archive mismatches remain failures even when a later export passes.

`cmake --build <build-dir> --target release-metadata` emits the same application
version as daemon/CLI. Metadata includes Git commit/tree/state, compiler, target,
configuration and actual per-network protocol/minimum-peer versions. Wall-clock
time is omitted (`build_timestamp: null`). A reviewed `SOURCE_DATE_EPOCH` may be
supplied at CMake configuration, recorded as UTC Unix seconds; the same value must
be used by both builders. Paths, usernames, credentials and private keys are not
metadata inputs. Archive-declared provenance remains explicitly unverified.

These are source-release preparation outputs, not complete executable packages,
an SBOM, license acceptance, operational approval or independent reproduction.
Compare independently built unsigned executables/packages using pinned complete
inputs; one source archive generated twice only proves archive repeatability.

## Custodian signatures — no CI secret material

Only the authorized custodian, in their own controlled environment after separate
signing approval, may run:

```sh
python3 -B scripts/release.py sign --manifest <directory>/SHA256SUMS \
  --output <directory>/SHA256SUMS.asc --fingerprint <APPROVED_PUBLIC_PRIMARY_FINGERPRINT>
gpg --verify <directory>/SHA256SUMS.asc <directory>/SHA256SUMS
```

The helper verifies payload hashes, refuses existing signature output and CI,
uses the local GPG agent/keyring, and verifies the resulting primary fingerprint.
It never imports/exports keys, supplies passphrases, dumps environment variables
or prints GPG stderr. Do not pass private keys or passphrases in CLI arguments,
Git, CI variables/logs or artifacts. CI validates unsigned code only, with
read-only repository permissions; no signing credentials are needed or printed.

A detached signature is not release approval. Tag creation, tag push, signing,
artifact publication, infrastructure and service startup each require their own
exact scope. This tool has no upload/push/deploy/activate operation and refuses
1.x preparation under TASK023. No mainnet lock or genesis is changed.
