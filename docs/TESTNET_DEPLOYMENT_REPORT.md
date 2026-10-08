# NovaCoin TESTNET deployment report

**Status:** **NOT DEPLOYED — TESTNET DISABLED**
**Report date:** 2026-10-08 UTC
**Implementation revision:** `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909`
**Internal engineering tag:** `task021-engineering-candidate-2b6f31f`

This is a factual readiness record, not a release announcement or approval.
It does not enable TESTNET and it does not change the independently disabled
MAINNET table.

## Candidate configuration

| Item | Candidate value |
| --- | --- |
| Network magic | `0xDAB5BFFB` |
| P2P / RPC port | `28333` / `28332` |
| P2PKH / private-key prefix | `112` / `240` |
| P2P / minimum peer protocol version | `1` / `1` |
| PoW limit compact | `0x2070ffff` |
| Target spacing / retarget interval | 600 seconds / 2016 blocks |
| Minimum-difficulty exception | enabled |
| Initial subsidy / halving interval | 5000000000 base units / 210000 blocks |
| Maximum money | `3100000000000000` base units (31,000,000 NOVA) |
| Maximum block size / transactions | 1000000 bytes / 256 |
| Genesis allocation / recipient | `1000000000000000` base units / P2PKH `37f3432cb47a3f078ed6351c5fa25d8cfed1ad64` |
| Genesis nonce | `2` |
| Genesis hash | `7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c` |
| Genesis Merkle root | `9c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a63` |

The full canonical genesis block, coinbase message, nonce, target, and digest
are committed in `docs/testnet-genesis-review.md`. Both
`TestnetNetworkParams().enabled` remains `false` and `.deployment_final` is
`true`. That finalization bit is non-activating and does not authorize a
deployment.

## Services

No public TESTNET service is deployed. There are no approved seed endpoints,
DNS records, faucet, public explorer, monitoring deployment, or public RPC
listener. The daemon binds RPC to loopback only; its `--testnet` startup path
refuses selection while the immutable table remains disabled.

Pre-approval templates now exist under `contrib/testnet/` for a non-root
container, compose deployment, systemd service, log rotation, secret
environment file, and a deliberately non-routable three-region bootstrap
manifest. They are not deployed services and must not be populated with
invented endpoints.

## Verification evidence

### Internal successor candidate and genesis gate — 2026-10-08 UTC

The signed internal engineering tag
`task021-engineering-candidate-2b6f31f` resolves to
`2b6f31f4f8aa9a3cb726dbdaa8069566afcab909`. The tag is signed by
`77F0948246E379FA7BF17F779C30AE406D9271F1` and was verified from a fresh
detached checkout. Issue #6 records the associated consensus-owner,
release-owner, and limited tag decisions. Those records close only the internal
immutable-genesis evidence gate; they do not authorize TESTNET activation,
deployment, infrastructure, release packages, or public launch.

### OCI candidate reproduction — 2026-09-22 UTC

Candidate `task021-engineering-candidate-89edf30` resolves to
`89edf303e93d20b16e4c33e362acccadef2273aa`. Its tag was verified with the
approved public key fingerprint
`77F0948246E379FA7BF17F779C30AE406D9271F1` on the private OCI validation
host. The redacted evidence archive SHA-256 is
`258094b1dc8ffa8725b05dc33669e7ea0650f020ef92c411a8207e7ee5d9ccaf`; its
outer and internal relative-path manifests were independently rechecked.

| Check | Result |
| --- | --- |
| Debug / Release build and CTest | passed |
| ASan / UBSan build and CTest | passed |
| Strict clang-tidy and clang-format | passed |
| Focused bootstrap and faucet tests | passed |
| Four-process Unix REGTEST harness | passed |
| Direct non-executable harness invocation | exit `126`; external wrapper-permission condition, not a storage finding |

This is single-host OCI validation only. It is not an independent operator
approval, a public deployment result, or full-chain UTXO verification.

### Bootstrap header-only configuration proposal — 2026-09-23 UTC

The parser-only, no-endpoint bootstrap proposal was committed separately as
`50e99c62980857d6bd9a29d6627eeeb1d1794271` (`testnet: allow header-only
bootstrap configuration`). It is intentionally separate from this report and
from the frozen engineering candidate. OCI content-equivalence verification
matched the three committed source objects, and the focused bootstrap suite
and `/usr/bin/clang-format-18` check passed. The associated post-commit
evidence remains single-host validation only.

### AWS successor validation — 2026-09-26 UTC

The reviewed successor revision
`346f64970d6f00c9c7d5f3402e92bf733e9cc5f6` was validated on the separate AWS
host using the external R4 driver. Debug, Release, ASan, UBSan, strict
clang-tidy, clang-format, focused bootstrap/faucet tests, the four-process
REGTEST harness, and valid-argument TESTNET/MAINNET refusal checks all passed.
The driver generated and verified a relative-path `SHA256SUMS` manifest.

This is a separate AWS machine validation only. Operator independence and a
detached signature remain pending; it is not an independent approval, public
deployment result, or full-chain UTXO verification.

### Independent review — 2026-09-24 UTC

GitHub user `alielhajj694-glitch` was granted Write access (not Admin) and
submitted a formal approval for pull request #1 at
`346f64970d6f00c9c7d5f3402e92bf733e9cc5f6`. GitHub Actions build-and-test,
ASan, and UBSan jobs passed for that head. This report update requires a
renewed review before merge. A detached operator attestation remains pending.

Current-source pre-approval checks:

| Check | Result |
| --- | --- |
| Clean WSL Clang Debug build | passed |
| Unit suite | passed |
| Four-process Unix regtest RPC harness | passed |
| clang-tidy build of affected targets | passed without diagnostics |
| clang-format and whitespace checks | passed |
| Direct `--testnet` / `--mainnet` startup gate probe | rejected before state-directory creation |

The earlier immutable candidate
`f0e46f12d8d1d0366e735b47d6783a87fadc70fa` has a successful hosted CI run:
[GitHub Actions run 32633882221](https://github.com/g8ht6jzdds-rgb/NovaCoin/actions/runs/32633882221).
Its retained strict-build/regtest, ASan/fuzz, and UBSan/fuzz artifacts expire
on 2026-09-22 UTC. That hosted evidence predates the current pre-approval
protocol-version parameterization and must be repeated for any future
enablement candidate.

## Security considerations and known limitations

- TESTNET is intentionally unavailable pending a separate activation decision
  and completion of the remaining operational launch gates.
- No DNS seed is implemented or advertised. A manually configured peer is
  still required for future bootstrap testing.
- No public faucet exists; any future faucet must be a separate service using
  TESTNET-only address decoding and an isolated encrypted hot wallet.
- No public explorer, release package, signing identity, or operator-managed
  monitoring service exists.
- Passing local or hosted tests is not a production-readiness claim.

## Required work before deployment

1. Complete the operational review in `docs/testnet-operations-review.md` with
   real named owners, dated evidence, and a rollback exercise.
2. Provision independently operated Europe, North America, and Asia seed
   nodes with real endpoints, removal procedures, ownership contacts, and no
   consensus privilege.
3. Establish separate monitored hosts and credentials for seed nodes,
   explorer, faucet, monitoring, and administration; keep public RPC off.
4. Implement and test a separately deployed faucet, release packaging/signing,
   approved bootstrap manifest, deployment assets, and a public TESTNET
   distributed validation exercise.
5. Run the complete clean validation matrix and hosted Unix CI from the final
   immutable enablement candidate, then make the separately reviewed
   `deployment_final=true` followed by `enabled=true` changes without changing
   the exact genesis vectors.

## Remaining operational requirements

- Reaudit the retained signed A/B genesis-reproduction evidence and its stated
  control-boundary limitation before any activation decision; a signature alone
  is not an independent operational audit.
- A complete chain UTXO inspection method and its independent evidence; wallet
  `listunspent` observations are insufficient.
- Three independently operated live seed nodes and, if enabled, DNS ownership
  and removal controls.
- A TESTNET-only faucet custody ceremony, documented dual-custodian emergency
  stop process, monitoring, and on-call ownership.
- A deployed public explorer with the required no-value banner and operational
  monitoring, plus signed Linux/Windows/macOS release artifacts and
  reproducible-build metadata.

## MAINNET blockers

MAINNET is **NOT FINAL — DO NOT DEPLOY**. It has no final reviewed parameters,
genesis commitment, independent reproductions, activation decision, public
operations review, signing provenance, or deployment authorization.
