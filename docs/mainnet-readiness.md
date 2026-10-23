# NovaCoin MAINNET readiness program — TASK022

**MAINNET LOCKED. NOT READY FOR REAL VALUE.** This is a readiness program, not a
launch proposal. TASK021's seedless development permissions and fixtures are not
public-testnet acceptance. No final MAINNET genesis is generated, no deployment
lock is removed, and no real-value service is started by this program.

## Admission and evidence rules

`nova::node::MAINNET_LOCKED` is an immutable true application-policy constant.
`SelectNetwork(kMainnet)` returns `kMainnetLocked` before loading its table.
`novacoind --mainnet` exits 1 with the reason before startup side effects, including
when other required arguments are missing. Existing MAINNET `enabled=false` and
`deployment_final=false` remain independent barriers. No flag, environment
variable, RPC or build option can unlock it. Standalone `--version` is preserved.
This is not protection against a person changing source code or an OS sandbox.

The only gate states are **PASS**, **FAIL**, **NOT TESTED**, **HUMAN REVIEW REQUIRED**.
All 14 gates are mandatory. Any non-PASS forbids activation; all-PASS still does
not remove the lock or authorize deployment. Final governance requires separate
reviewed source change and explicit authorization, outside TASK022.

Each result must bind the exact commit AND tested dirty patch/tree digest, network,
tools/dependencies, commands, exit codes, duration, logs, manifests, test names,
coverage limitations and responsible reviewers. CI checkmarks at a parent commit
are not evidence for new source. Deterministic synthetic/REGTEST evidence is not
operational TESTNET, external review, independent reproduction or readiness.

## Required gates and closing evidence

| Gate | Mandatory acceptance |
| --- | --- |
| 1 Consensus freeze | Versioned complete normative spec and parameter manifest, rule checklist, ambiguity/discrepancy resolution, signed human consensus-owner/reviewer freeze. See consensus-freeze.md. |
| 2 Consensus coverage | Every normative rule mapped to implementation and named positive, negative AND boundary assertions. Missing cells fail, not skipped. See consensus-coverage.md. Reference existence is not proof of adequate coverage. |
| 3 Determinism | Identical corpus in Debug/O0, Release/O2 and O3, repeated runs, every supported OS/compiler CI combination; compare block hashes, txids, full sorted UTXO checksum, active tip and exact chainwork. Record failures and unsupported platforms. |
| 4 Money conservation | Ordinary signed transaction admission rejects one-unit creation; coinbase ceiling/fee/amount overflow and theoretical issuance tests including genesis allocations; no floating point or unchecked overflow. Empirical tests are not a mathematical proof for all possible inputs. |
| 5 Reorganization safety | Bounded 1/10/100-block REGTEST fork switches via ordinary ChainState, exact UTXOs verified after EVERY accepted block, losing outputs absent, invalid extension leaves state unchanged; signed-spend undo/reorg fixtures retained. Deep fixture is coinbase-only, so do not claim deep signed dependency coverage. |
| 6 Database safety | Fault-injected I/O AND abrupt-process crashes during prepare/write, UTXO publication, tip publication, wallet replacement and shutdown, replay idempotence/full state checks; include disk-full/flush/rename limitations. Existing journal injected write errors do not prove all actual crash points. |
| 7 Adversarial input | Headers, txs, blocks, inventory, addresses, lengths, varints, signatures, compact difficulty: reject malformed/overflow/noncanonical/oversize data with bounded resources and exact before/after valid state. No crash/UB/state mutation. |
| 8 Fuzzing | Inventory EVERY externally controlled parser and target/gap. Retain exact target/tool/runtime/executions/crashes/seed/input caps and coverage when available, not just successful build. Short smoke is not sufficient security assurance. |
| 9 Analysis | Strict warnings (MSVC /W4 /WX; Clang/GCC Wall/Wextra/Wpedantic/Wconversion/Wsign-conversion/Werror), clang-tidy, ASan, UBSan, TSan where practical; consensus findings block until independently verified dispositions. Unsupported tool/runtime is NOT TESTED, not PASS. |
| 10 External security review | Real external humans with actual separate control, scope/findings/fixes/retests and exact reviewed revision. Never auto-complete. See ../security/audits/scope.md. |
| 11 Long public testnet | Stable public testnet, genuinely independent operators, upgrades, reorg/recovery, wallet and propagation evidence, incident history and reviewer-approved meaningful duration. No arbitrary day count automatically sufficient. |
| 12 Reproducible releases | Two independently controlled builders, pinned complete inputs and licensing/SBOM, signed identity, compared unsigned artifacts for each supported target. Shared-control repeat builds are not independent. See release-process.md. |
| 13 Configuration review | Candidate-only values/rationale, economic/security review, genesis allocation/maturity/issuance reconciliation, identity/port collisions and upgrade compatibility. No final genesis or activated parameter table. |
| 14 Release approval | All gates PASS at exact immutable candidate, current human approvals and evidence accessibility, incident/custody/rollback and exact publication scope accepted. No automated approval or MAINNET activation. |

## Deterministic corpus and repeatable checks

`nova_readiness_vectors` is an offline bounded REGTEST fixture, using fixed time,
bounded nonce search and fixed coinbase bytes. It exercises 1/10/100-deep branches
and compares all active fixture coins after every block. It emits three lines:
depth|height|coin-count|block-hash-list-checksum|txid-list-checksum|UTXO-checksum|
tip|raw-512-bit-chainwork. Hex uses raw byte order. UTXO checksum is DOUBLE-SHA256
of sorted `(txid,u32 index,i64 value,u32 height,u8 coinbase,CompactSize script)`;
this is a test checksum, NOT a new consensus UTXO-root commitment.

Build identical source/dependencies in independent build directories. Capture
`nova_readiness_vectors` output and compare entire bytes across configs/OS. A
reference generated by the same implementation only detects divergence, not a
shared consensus bug. Extend corpus with signed transactions, retarget boundaries,
wallet-independent spend graphs and independent reference vectors before Gate 3
can pass for all behavior. Existing tests still verify their own canonical vectors.

Commands (local paths/configurations must be explicit in retained evidence):

```sh
cmake --preset debug -DNOVA_ENABLE_CLANG_TIDY=ON
cmake --build --preset debug --parallel 2
ctest --preset debug --output-on-failure
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --output-on-failure
cmake -P scripts/check-format.cmake
python3 -B scripts/check_mainnet_readiness.py
python3 -B scripts/check_mainnet_readiness.py --activation-check # MUST refuse
```

Run sanitizer/fuzz presets with retained limits (e.g. max input 4096, RSS 512 MiB,
fixed seed, bounded runtime). Linux `nova.readiness_vectors` and lock tests are
offline; existing CTest integration uses loopback REGTEST only. No TESTNET/MAINNET
daemon is required. Never execute `nova-genesis` to produce a final MAINNET block.
Policy inventory is not a signed gate acceptance. Current states live in
`../MAINNET_CHECKLIST.md`; final observed results in `../MAINNET_READINESS_REPORT.md`.

## Candidate MAINNET parameters — review only, never activated

These are the EXISTING non-final compiled placeholders from `docs/networks.md`,
not new approved economic choices. Each value needs human rationale acceptance,
collision/threat/economic analysis and exact-vector review. No table/genesis is
changed by TASK022. The final MAINNET genesis is deliberately unspecified.

| Parameter | Existing provisional candidate | Rationale / unresolved review |
| --- | --- | --- |
| Network magic | 0xDAB5BFFC | Distinct from development networks; not a reserved globally collision-free identity; peer ecosystem review required. |
| P2P/RPC ports | 39333 / 39332 | Distinct local defaults; availability/conflicts and exposure policy unreviewed; RPC must remain authenticated loopback. |
| Address encoding | Base58Check P2PKH prefix 68, private prefix 128 | Distinct NovaCoin public address prefix; private prefix may overlap other chains and key-import confusion needs review. No wallet key migration implied. |
| Block interval | 600 seconds | Existing Bitcoin-inspired target; throughput, propagation, security and operator measurements needed. |
| powLimit | canonical compact 0x2060ffff | Very easy DEVELOPMENT placeholder, not a justified real-value security threshold; hash-power/launch economics unresolved. |
| Difficulty | 2016 blocks / 1,209,600 seconds, integer quarter-to-four-times clamps, no min-difficulty exception | Existing v0 policy; hash-rate shocks, startup and adversarial timestamp analysis required. No algorithm approved for launch. |
| Initial block reward | 50 NOVA / 5,000,000,000 atomic units | Existing educational schedule; economic/distribution review pending. |
| Halving | 210,000 blocks | Existing integer schedule, verify full issuance and long-term security assumptions. |
| Maximum supply | MAX_MONEY = 31,000,000 NOVA | Schedule is about 21M; remaining headroom is not permission to allocate 10M on MAINNET. Genesis distribution and exact total need explicit review. TESTNET allocation does not transfer. |
| Block/transaction limits | 1,000,000 bytes, 256 tx/block; 100,000 bytes/tx; 128 inputs/outputs; 256-byte scripts; 100-byte coinbase script | Existing explicit bounds; DoS/validation/propagation/capacity analysis required. Current weight equals serialized size, no implicit witness policy. |
| Genesis / allocation / maturity | NO FINAL MAINNET CHOICE | Existing NOT FINAL fixture is not an activation commitment. No mining/generation/finalization performed. |
| Deployment gates | enabled=false, deployment_final=false, MAINNET_LOCKED=true | Three separate barriers remain; no document or test removes them. |

## Long-running public TESTNET acceptance (Gate 11)

Owner and external reviewers must first approve exact environments, identities,
budgets, permitted exposure and acceptance criteria. Then retain dated continuous
uptime/incident/chain-progress records, actual independently controlled regional
operators (not merely different accounts), version/config inventories, authorized
upgrade/rollback/reorg/recovery exercises, wallet funding/spends, block/transaction
propagation latency/failure distributions, monitoring/on-call and data integrity.
Public TESTNET coins have no value. Private REGTEST, short benchmarks and mock
services do not close this gate. A duration must be explicitly justified and
accepted against real workloads/incidents; no automatic N-days sufficiency rule.
The current seedless candidate and owner-reported common control do not satisfy it.
