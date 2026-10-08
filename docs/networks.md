# NovaCoin network parameters and genesis fixtures

`NetworkParams` is an immutable compiled-in application/consensus parameter
table.  It owns the network identity, transport ports, address presentation
prefixes, monetary table, proof-of-work table, difficulty policy, and genesis
block commitment.  No caller may combine fields from different networks.

| Network | P2P magic | P2P port | RPC port | P2P / minimum peer version | P2PKH prefix | PoW limit compact | spacing | halving | status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| REGTEST | `0xDAB5BFFA` | 18444 | 18443 | 1 / 1 | 111 | `0x207fffff` | 600 s | 150 | enabled for local development |
| TESTNET | `0xDAB5BFFB` | 28333 | 28332 | 1 / 1 | 112 | `0x2070ffff` | 600 s | 210000 | internal candidate finalized; activation and public deployment pending |
| MAINNET | `0xDAB5BFFC` | 39333 | 39332 | 1 / 1 | 68 | `0x2060ffff` | 600 s | 210000 | **NOT FINAL — DO NOT DEPLOY** |

REGTEST has `no_retargeting=true`; TESTNET permits minimum-difficulty blocks;
MAINNET's difficulty policy is present only as a non-final placeholder. The
initial subsidy is 50 NOVA and the supply cap is 31,000,000 NOVA. TESTNET has
a 10,000,000-NOVA genesis P2PKH allocation to
`37f3432cb47a3f078ed6351c5fa25d8cfed1ad64`, while its scheduled subsidy
issuance remains bounded by 21,000,000 NOVA. Address prefixes are presentation
data; they do not alter the P2PKH consensus script.

All current tables use these explicit primitive limits: maximum transaction
size 100,000 bytes, 128 inputs, 128 outputs, 256-byte scripts, 1,000,000-byte
blocks, 256 transactions per block, and 100-byte coinbase scripts.  The
private-key presentation prefixes are 239 (REGTEST), 240 (TESTNET), and 128
(MAINNET).

| Network | timestamp | message | reward | nonce | raw genesis hash | raw Merkle root |
| --- | ---: | --- | ---: | ---: | --- | --- |
| REGTEST | 1704067200 | `NovaCoin Regtest Genesis` | 5000000000 | 3 | `c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79` | `532198bb92e48c7059ddcb818874d5993a19fc6ee5c2809ff27e6bb07479a146` |
| TESTNET | 1704153600 | `NovaCoin Testnet Genesis` | 1000000000000000 | 2 | `7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c` | `9c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a63` |
| MAINNET | 1704240000 | `NovaCoin Mainnet NOT FINAL` | 5000000000 | 0 | `d782facabba1095d7fe0338aa8dfb3f98dd5057de258848db0376c6a99e9b323` | `7a05700b18bae809cda367da34b74019a3038d2ffb850d7b98ee5015f77797d9` |

The MAINNET row is a provisional development fixture only: **NOT FINAL — DO
NOT DEPLOY**.  It is not an authority to enable mainnet, launch a chain, or
reserve any network identity.

## Genesis construction

`nova-genesis` deterministically builds one v0 block from:

```text
--timestamp <u32-seconds>
--message <printable ASCII, 1..64 bytes>
--target <canonical compact target as eight hexadecimal digits>
--reward <base-unit integer>
[--recipient-p2pkh <40-hex>]
```

The coinbase has one null outpoint, a script consisting of the little-endian
timestamp followed by the ASCII message, and one `OP_TRUE` output carrying the
specified reward.  When `--recipient-p2pkh` is present, the output is the
canonical P2PKH script for that 20-byte hash; otherwise it is `OP_TRUE`. The header has version 1, an all-zero parent, the computed
transaction Merkle root, supplied time and target, and the first nonce from
zero that satisfies the target.  The utility rejects noncanonical, zero,
negative, overflowing, or above-network-independent `uint256` targets, invalid
amounts, and impossible/oversized messages.

The REGTEST and TESTNET fixtures are compiled in and exact tests verify their
block hashes and Merkle roots. The TESTNET candidate additionally requires the
review/approval artifact in `docs/testnet-genesis-review.md`; its table stays
disabled until a separate approved enablement change. MAINNET is intentionally not an activation
commitment: its table is disabled and marked **NOT FINAL — DO NOT DEPLOY**.
Changing any generated fixture requires a reviewed parameter update and new
exact vectors; it is never a run-time configuration choice.
