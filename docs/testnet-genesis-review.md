# NovaCoin testnet genesis review and approval record

**Artifact ID:** `NOVACOIN-TESTNET-GENESIS-001`  
**Status:** **INTERNAL CANDIDATE FINALIZED — TESTNET DISABLED — PUBLIC OPERATIONS/ACTIVATION PENDING**
**Last reviewed source revision:** `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909` (`task021-engineering-candidate-2b6f31f`)

This is the release-gate record for a public NovaCoin testnet genesis block.
It distinguishes the compiled internal candidate from a final public deployment
commitment. No field below authorizes a launch.
No code reads this document to determine consensus validity or enablement.

This finalization-review candidate changes only the review gate. Until every
approval item is completed and a distinct activation change is reviewed, the
compiled table must remain:

```text
TestnetNetworkParams().enabled          == false
TestnetNetworkParams().deployment_final == true
```

`CheckNetworkParams` also rejects a testnet table whose `enabled` bit is true
while `deployment_final` is false. This is a defense against an accidental
one-bit activation; it does not replace the human review recorded here. The
`deployment_final` value alone does not authorize a launch or TESTNET startup.
MAINNET has an independent, stricter **NOT FINAL — DO NOT DEPLOY** gate.

## Immutable internal-candidate network parameters

These values are copied from `src/consensus/network_params.cpp` at the reviewed
revision. They are immutable for the internal candidate but do not authorize
activation or public deployment.

| Parameter | Candidate value |
| --- | --- |
| Network ID | `TESTNET` |
| Network magic | `0xDAB5BFFB` |
| Default P2P port | `28333` |
| Default RPC port | `28332` |
| P2P protocol / minimum peer version | `1` / `1` |
| P2PKH / private-key presentation prefixes | `112` / `240` |
| PoW compact limit | `0x2070ffff` |
| Decoded PoW limit, big-endian | `70ffff0000000000000000000000000000000000000000000000000000000000` |
| Target spacing | `600` seconds |
| Retarget interval / timespan | `2016` blocks / `1209600` seconds |
| Minimum-difficulty exception | enabled |
| No-retargeting | disabled |
| Halving interval | `210000` |
| Initial subsidy / maximum money | `5000000000` / `3100000000000000` base units (31,000,000 NOVA) |
| Block limits | `1000000` bytes, `256` transactions |

## Immutable internal-candidate genesis commitment

All hex values below use NovaCoin's raw canonical-byte display order. They
must be compared byte-for-byte, not interpreted as host-order integers.

| Field | Candidate value |
| --- | --- |
| Header version | `1` |
| Previous block hash | 32 zero bytes |
| Timestamp | `1704153600` |
| Coinbase message, printable ASCII | `NovaCoin Testnet Genesis` |
| Coinbase reward | `1000000000000000` base units (10,000,000 NOVA) |
| Coinbase output | P2PKH `37f3432cb47a3f078ed6351c5fa25d8cfed1ad64` |
| Header bits | `0x2070ffff` |
| First valid nonce | `2` |
| Generator attempts | `3` |
| Merkle root | `9c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a63` |
| Genesis block hash | `7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c` |

The candidate proof of work is valid only when the canonical header hash is
compared using the documented NovaCoin little-endian hash interpretation
against the decoded target. Reviewers must independently verify this rather
than relying on a display string.

## Reproduction procedure

Use a clean checkout, the pinned C++20/CMake toolchain, and the exact source
revision recorded in the approval table. Build `nova-genesis`, then run:

```sh
nova-genesis \
  --timestamp 1704153600 \
  --message "NovaCoin Testnet Genesis" \
  --target 2070ffff \
  --reward 1000000000000000 \
  --recipient-p2pkh 37f3432cb47a3f078ed6351c5fa25d8cfed1ad64
```

Expected output:

```text
nonce=2
hash=7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c
merkle_root=9c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a63
block_hex=0100000000000000000000000000000000000000000000000000000000000000000000009c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a6300529365ffff7020020000000101000000010000000000000000000000000000000000000000000000000000000000000000ffffffff1c005293654e6f7661436f696e20546573746e65742047656e6573697300000000010080c6a47e8d03001976a91437f3432cb47a3f078ed6351c5fa25d8cfed1ad6488ac00000000
block_sha256d=a44b52e7067d9724b2d2086ca5a015cf9e6f564f8a0c194c90465d40599dfc74
attempts=3
```

On Windows, reviewers MUST use `scripts/verify_testnet_genesis.ps1`; it checks
that the requested 40-hex source revision is the clean checkout's `HEAD`, then
validates the pinned CMake, LLVM, vcpkg baseline, and installer checksums,
builds `nova-genesis`, compares the complete candidate commitment, and writes a
non-overwritable evidence file. It rejects labels such as `HEAD`, `pending`,
uncommitted trees, historical commits not currently checked out, and unrelated
untracked files.

```powershell
./scripts/verify_testnet_genesis.ps1 `
  -SourceRevision <40-hex-committed-revision> `
  -EvidencePath docs/testnet-genesis-evidence/reproduction-a-<revision>.txt
```

Reproducer B must use a separately controlled environment and write a distinct
`reproduction-b-<revision>.txt` file. Both evidence files must be reviewed in
the approval change. Copying Reproducer A's output, rerunning it on the same
environment, or using an uncommitted source tree does not constitute an
independent reproduction.

## Mandatory approval checklist

All boxes require evidence in the pull request or release review. “Pending” is
not approval.

| Gate | Required evidence | Status |
| --- | --- | --- |
| Independent reproduction A | Clean pinned-toolchain command, source revision, full serialized block hex, hash, and Merkle root | Signed record verified; declared control boundary is not independently audited |
| Independent reproduction B | Same artifacts produced independently by a second maintainer | Signed record verified; declared control boundary is not independently audited |
| Parameter review | Magic, ports, prefixes, monetary table, limits, and difficulty policy checked for collisions and intended behavior | Consensus-owner decision recorded on Issue #6 |
| PoW and serialization review | Canonical encoding, compact-target canonicality, nonce, raw hash order, Merkle root, and PoW comparison checked | Exact vectors and post-merge compact-target regression reviewed; see Issue #6 and tag provenance |
| Test evidence | Exact candidate vectors pass; altered hash, root, target, and activation-without-finalization tests fail | Exact-vector tests and exact-commit CI recorded; not a public-network test |
| Public testnet operational review | Seed/bootstrap, monitoring, release rollback, abuse handling, and disclosure plan approved | Pending |
| Explicit enablement decision | Separate reviewed commit sets `deployment_final=true` and then `enabled=true`; release owner records its revision | Pending |

## Historical pre-allocation engineering evidence (not current approval evidence)

The following 2026-08-22 local evidence predates the committed creator
allocation and current genesis vectors. It is preserved as historical context
only. It does **not** establish either independent reproduction, validate the
current immutable candidate, or authorize testnet deployment.

| Area | Evidence | Result |
| --- | --- | --- |
| Parameter table | `NetworkParams.CommitsDistinctRegtestAndTestnetFixtures`, `MatchesExactTestnetGenesisHashAndMerkleRoot`, and `RejectsTestnetActivationWithoutFinalApprovalFlag` | Passed in the 103-test Debug suite |
| Canonical serialization | `NetworkParams.CommitsExactTestnetGenesisSerializationEvidence` pins the full block bytes and SHA-256d `04678f985ea3de5a84dffa8536218b65e599950c114034855e18434003014d63` | Passed in the 103-test Debug suite |
| PoW and generator | `GenesisGenerator.ReproducesTestnetCandidateFixture` and `nova-genesis` built with CMake 4.3.3 / MSVC 19.44.35228 produced nonce `0`, the committed candidate hash, and the committed Merkle root | Matched candidate fixture |
| Toolchain guard | `scripts/verify_testnet_genesis.ps1` verifies installer checksums, CMake 4.3.3, LLVM 20.1.8, MSVC 19.44.35228, and the vcpkg baseline before writing evidence | Implemented; requires a real Git commit |
| Negative activation control | Candidate TESTNET has `enabled=false` and `deployment_final=true`; `CheckNetworkParams` rejects one-bit activation without finalization | Passed |

Historical pre-approval output produced before a committed source revision is
deliberately excluded from the approval record. Only a reproduction naming an
immutable 40-hex commit and satisfying the independent-environment procedure
above may be considered for the approval table.

## Operational-review evidence required

The release owner must complete and approve
`docs/testnet-operations-review.md` before the public-testnet operational
review can move from Pending. That record must contain real owners, dates,
revision references, and the approved values for all listed controls.

## Approval record

Do not mark this section approved until every mandatory gate above is complete.
Names, dates, and revision identifiers must be real review evidence; placeholders
are forbidden.

| Role | Name | Date (UTC) | Source revision | Signature/reference |
| --- | --- | --- | --- | --- |
| Reproducer A | ALI NOUR EL HAJJ | 2026-10-08 | `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909` | Signed record and control-boundary declaration audited outside the repository |
| Reproducer B | Ali El Hajj | 2026-10-08 | `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909` | Signed record and control-boundary declaration audited outside the repository |
| Consensus reviewer | `ane23-dot` | 2026-10-08 | `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909` | https://github.com/g8ht6jzdds-rgb/NovaCoin/issues/6#issuecomment-6061539588 |
| Release owner | `anemee-coder` | 2026-10-08 | `2b6f31f4f8aa9a3cb726dbdaa8069566afcab909` | https://github.com/g8ht6jzdds-rgb/NovaCoin/issues/6#issuecomment-6061588246 |
| Explicit enablement commit | Pending | Pending | Pending | Pending |

## Assigned review roles

These are role assignments only; they are not approvals, signatures, or
evidence of independent review. Dates, immutable revision references, and
approval signatures remain required in the table above.

| Review role | Assigned owner | Status |
| --- | --- | --- |
| Consensus/parameter owner | `ane23-dot` | Decision recorded on Issue #6; no activation authorization |
| Release owner | `anemee-coder` | Decision recorded on Issue #6; no activation authorization |

## Activation rule

After approval, activation still requires a separate, reviewed source change
that updates the immutable testnet table and exact regression vectors together.
That change must set `deployment_final=true` before `enabled=true`, preserve
all approved bytes exactly, and state the authorization reference. It must not
change MAINNET status. Until that change lands, TESTNET remains disabled and
this document is not public-launch authorization.
