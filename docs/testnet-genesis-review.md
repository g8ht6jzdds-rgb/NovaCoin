# NovaCoin testnet genesis review and approval record

**Artifact ID:** `NOVACOIN-TESTNET-GENESIS-001`  
**Status:** **PENDING — CANDIDATE ONLY — TESTNET DISABLED**  
**Last reviewed source revision:** Pending clean, pinned-toolchain reproduction

This is the release-gate record for a public NovaCoin testnet genesis block.
It deliberately distinguishes the currently compiled *candidate fixture* from
a final, approved deployment commitment. No field below authorizes a launch.
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

## Candidate immutable network parameters

These are candidate values copied from
`src/consensus/network_params.cpp`. They are **not final** until the approval
record below is complete.

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
| Initial subsidy / maximum money | `5000000000` / `2100000000000000` base units |
| Block limits | `1000000` bytes, `256` transactions |

## Candidate genesis commitment

All hex values below use NovaCoin's raw canonical-byte display order. They
must be compared byte-for-byte, not interpreted as host-order integers.

| Field | Candidate value |
| --- | --- |
| Header version | `1` |
| Previous block hash | 32 zero bytes |
| Timestamp | `1704153600` |
| Coinbase message, printable ASCII | `NovaCoin Testnet Genesis` |
| Coinbase reward | `5000000000` base units |
| Header bits | `0x2070ffff` |
| First valid nonce | `0` |
| Generator attempts | `1` |
| Merkle root | `e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c` |
| Genesis block hash | `25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048` |

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
  --reward 5000000000
```

Expected candidate output:

```text
nonce=0
hash=25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048
merkle_root=e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c
block_hex=010000000000000000000000000000000000000000000000000000000000000000000000e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c00529365ffff7020000000000101000000010000000000000000000000000000000000000000000000000000000000000000ffffffff1c005293654e6f7661436f696e20546573746e65742047656e65736973000000000100f2052a01000000015100000000
block_sha256d=04678f985ea3de5a84dffa8536218b65e599950c114034855e18434003014d63
attempts=1
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
| Independent reproduction A | Clean pinned-toolchain command, source revision, full serialized block hex, hash, and Merkle root | Pending |
| Independent reproduction B | Same artifacts produced independently by a second maintainer | Pending |
| Parameter review | Magic, ports, prefixes, monetary table, limits, and difficulty policy checked for collisions and intended behavior | Pending |
| PoW and serialization review | Canonical encoding, compact-target canonicality, nonce, raw hash order, Merkle root, and PoW comparison checked | Pending |
| Test evidence | Exact candidate vectors pass; altered hash, root, target, and activation-without-finalization tests fail | Pending |
| Public testnet operational review | Seed/bootstrap, monitoring, release rollback, abuse handling, and disclosure plan approved | Pending |
| Explicit enablement decision | Separate reviewed commit sets `deployment_final=true` and then `enabled=true`; release owner records its revision | Pending |

## Pre-approval engineering evidence (not an approval)

The following evidence was generated in the local development workspace on
2026-08-22. It demonstrates that the candidate is internally reproducible; it
does **not** establish either independent reproduction and does not authorize
testnet deployment.

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
| Reproducer A | Pending | Pending | Pending | Pending |
| Reproducer B | Pending | Pending | Pending | Pending |
| Consensus reviewer | Pending | Pending | Pending | Pending |
| Release owner | Pending | Pending | Pending | Pending |
| Explicit enablement commit | Pending | Pending | Pending | Pending |

## Assigned review roles

These are role assignments only; they are not approvals, signatures, or
evidence of independent review. Dates, immutable revision references, and
approval signatures remain required in the table above.

| Review role | Assigned owner | Status |
| --- | --- | --- |
| Consensus/parameter reviewer | ALI NOUR EL HAJJ | Assigned; independent review pending |
| PoW and serialization reviewer | ALI NOUR EL HAJJ | Assigned; independent review pending |
| Security reviewer | ALI NOUR EL HAJJ | Assigned; independent review pending |
| Release owner | ALI NOUR EL HAJJ | Assigned; release decision pending |

## Activation rule

After approval, activation still requires a separate, reviewed source change
that updates the immutable testnet table and exact regression vectors together.
That change must set `deployment_final=true` before `enabled=true`, preserve
all approved bytes exactly, and state the authorization reference. It must not
change MAINNET status. Until that change lands, this document remains
**PENDING — TESTNET DISABLED**.
