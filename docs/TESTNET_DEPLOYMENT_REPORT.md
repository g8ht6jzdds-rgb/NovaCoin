# NovaCoin TESTNET deployment report

**Status:** **NOT DEPLOYED — TESTNET DISABLED**
**Report date:** 2026-08-23 UTC
**Implementation revision:** `2a8c9d875221ca15542af719ee4d5582bf76696e`

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
| Maximum block size / transactions | 1000000 bytes / 256 |
| Genesis hash | `25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048` |
| Genesis Merkle root | `e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c` |

The full canonical genesis block, coinbase message, nonce, target, and digest
are committed in `docs/testnet-genesis-review.md`. Both
`TestnetNetworkParams().enabled` and `.deployment_final` remain `false`.

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

- TESTNET is intentionally unavailable until independent genesis reproduction
  and named reviews are complete.
- No DNS seed is implemented or advertised. A manually configured peer is
  still required for future bootstrap testing.
- No public faucet exists; any future faucet must be a separate service using
  TESTNET-only address decoding and an isolated encrypted hot wallet.
- No public explorer, release package, signing identity, or operator-managed
  monitoring service exists.
- Passing local or hosted tests is not a production-readiness claim.

## Required work before deployment

1. Obtain two genuinely independent pinned-toolchain genesis reproductions
   and complete the named approval tables in
   `docs/testnet-genesis-review.md` and `docs/testnet-operations-review.md`.
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

- A second independently controlled, detached and signed reproduction record.
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
