# TASK022 — MAINNET readiness report

**NOT READY. MAINNET LOCKED. NO LAUNCH AUTHORIZATION.**

Assessment date: 2026-10-23. Engineering base:
`cf4523f4cf8b2f4c53511f62fef5e215203dd096`. The initial assessment tested an
uncommitted TASK022 snapshot on `codex/task022-mainnet-readiness`, not the unchanged
base or an immutable release. Initial source/file hashes and command logs are retained
outside the repository in `C:\NovaCoin-validation\task022-mainnet-readiness`.
The Linux validation copy is `/home/maher/task022-mainnet-readiness` in Ubuntu WSL.
The frozen initial source, logs and verified relative manifest are at
`C:\NovaCoin-evidence\task022-mainnet-readiness-20261023`.
At that assessment no hosted checks had run. Review corrections, submitted head
and new exact-head CI results are recorded separately in
`C:\NovaCoin-evidence\task022-pr-review-20261023`; never substitute parent results.

## Delivered safeguards and program

- Explicit compiled `nova::node::MAINNET_LOCKED=true`, checked both before daemon
  startup side effects and at network selection. No runtime or build override.
- Existing MAINNET `enabled=false` and `deployment_final=false` preserved. No
  consensus parameters, frozen TESTNET genesis or final MAINNET genesis changed.
- Four bounded startup-refusal controls, including conflicting arguments, missing
  paths and an ineffective environment-variable override.
- Consensus specification checklist, 25-group implementation/test matrix, monetary
  boundary tests and offline 1/10/100-block fork/reorganization fixtures checking
  every selected UTXO after every accepted block and invariance on invalid input.
- Deterministic block/transaction/UTXO/tip/chainwork vectors, a comparison workflow,
  strict fail-closed checklist inventory, release/security policies, external audit
  scope, public-testnet acceptance criteria and candidate parameter rationales.

Documents are a program and evidence inventory, not automatic gate acceptance.
The authoritative gate tokens are in [MAINNET_CHECKLIST.md](MAINNET_CHECKLIST.md).

## Validation record

Validation is bounded, synthetic/offline or loopback REGTEST. No TESTNET or MAINNET
service was started. No real keys, chain inputs, public endpoints or final genesis
generation were used. Existing tests may construct non-final genesis fixtures.

| Check | Observed result / scope |
| --- | --- |
| Strict Linux Debug and clang-tidy | Build passed; full CTest 20/20 passed after the documented corrections below. |
| Linux Release/O2, O3 determinism | Release build and CTest 20/20 passed. Debug/O0, Release/O2 and O3 vector outputs are byte-identical for the three offline fork fixtures. This is not cross-OS or exhaustive consensus determinism. |
| ASan / UBSan | Both builds and their full CTest suites passed 19/19 each (version preload guard is intentionally excluded in sanitizer configurations). |
| TSan | Instrumented build and all 223 unit tests passed; no race report observed. This did not run the separate multi-process CTest harnesses under TSan. |
| Existing 12 fuzz targets | All 12 built and completed 1,000 executions each, no crashes observed. Empty-start short smoke is not complete parser coverage or a security campaign. Details below. |
| Windows full Debug | FAILED: MSVC C1060 out of heap space while compiling existing RPC/address test translation units. A subsequent one-worker focused build also failed linking the daemon with LNK1102 out of memory. Neither a full Windows pass nor cross-OS vector equality is claimed. |
| macOS | NOT TESTED: no macOS build or runner was executed. The prepared hosted workflow covers Ubuntu only; platform support is not inferred. |
| Hosted CI | New workflow prepared, not executed. Historical parent/PR results do not validate this patch. |
| Formatting/syntax/policy | Full C++ clang-format-18 check, all shell-script syntax, changed Python syntax, workflow YAML structure and gate/activation-refusal tests passed. |
| Tested-input binding | All 13 changed code/test/build files match the Linux copy by SHA-256. Final documentation is separately retained in the source manifest. |

### Bounded fuzz observations

Clang 18, ASan+UBSan, fixed seed 22, maximum 1,000 executions, five-second
libFuzzer cap, 15-second driver timeout, maximum input 4,096 bytes and RSS cap
512 MiB per process. Started without retained seed corpora: evolved input limits
were only 6–11 bytes in these runs, not comprehensive large-message exploration.
Reported runtime is libFuzzer's whole-second value; driver elapsed was 0–1 seconds.
`cov`/`ft` are instrumented edge/feature counts, NOT percentages or full library
coverage. Existing separately compiled libraries do not all carry libFuzzer
coverage instrumentation; see docs/fuzz-inventory.md.

| Target | Executions | Reported runtime | Crashes | cov / ft |
| --- | ---: | ---: | ---: | ---: |
| fuzz_address_parser | 1000 | 0 s | 0 | 2 / 2 |
| fuzz_block_deserializer | 1000 | 0 s | 0 | 4 / 4 |
| fuzz_body_request_recovery | 1000 | 0 s | 0 | 59 / 175 |
| fuzz_bootstrap_reader | 1000 | 0 s | 0 | 13 / 14 |
| fuzz_compact_target | 1000 | 0 s | 0 | 6 / 6 |
| fuzz_deserializer | 1000 | 0 s | 0 | 26 / 43 |
| fuzz_network_message | 1000 | 0 s | 0 | 6 / 7 |
| fuzz_offline_inspection | 1000 | 0 s | 0 | 164 / 185 |
| fuzz_peer_discovery | 1000 | 0 s | 0 | 16 / 17 |
| fuzz_script_signature | 1000 | 0 s | 0 | 2 / 2 |
| fuzz_sync_admission | 1000 | 0 s | 0 | 155 / 426 |
| fuzz_transaction_deserializer | 1000 | 0 s | 0 | 4 / 4 |

Preserved failures and environment limitations:

1. Initial Linux CTest: 17/20. Peer-discovery expected the old MAINNET-disabled
   diagnostic; the test now asserts the exact new lock reason, without weakening
   the refusal requirement. Two shell harnesses received CRLF in the Windows-to-
   Linux transport copy. Later transport also lost script contents; the cause was
   not established. Windows source remained intact. The failed copy was preserved.
2. Verified source archive restored bytes with per-file SHA-256 verification.
   Linux shell line endings were normalized and two tracked executable modes
   restored. The intermediate 19/20 run failed with exit 126 because the collector
   had lost its executable bit. The corrected full suite passed 20/20.
3. Initial full Debug build exited successfully, but its original transient build
   log was unavailable later; the retained incremental strict-build and CTest logs
   must not be described as a retained clean-build log.
4. Windows full build encountered the memory failure above. Linux matrix builds
   use one worker to limit pressure. Host memory/disk limits are not consensus
   defects; neither do they excuse or erase unavailable validation.
5. Debug uses Clang 18 with the installed unversioned clang-tidy resolving to
   LLVM 21.1.8. The version-18 tidy identity in the general tool inventory is NOT
   the executable selected by that CMake cache; the actual selection is retained
   separately in `actual-static-toolchain.log`.
6. Linux validation tools: Clang 18.1.8, CMake 4.2.3, Ninja 1.13.2,
   libsecp256k1 0.7.0, OpenSSL 3.5.5; actual static analyzer 21.1.8 as above.
   These are installed validation inputs, not a clean-image release pin or the
   historical Windows experimental OpenSSL artifact. The unit executable reports
   223 tests in 65 suites; CTest counts include separate process/harness entries.

Matrix commands, exit codes and coarse elapsed seconds are retained in
`linux/matrix-status.tsv` and `linux/linux-matrix-runner.sh` in the consolidated
packet. All matrix entries exited 0. That does not erase the earlier transport,
assertion and Windows failures. No ASan/UBSan/TSan diagnostic was observed in the
retained successful runs. Polling/configuration-boundary disk checks are a stop
safeguard, not an OS-enforced quota; final observed host free space exceeded 10 GiB.

## Gate disposition and blockers

The checklist is deliberately not all PASS. No external or owner-controlled
fact has been manufactured. Passing tests are scoped empirical evidence, not a
proof that all possible consensus inputs or real-value deployments are safe.

Passing gates: **4, 5**, for their recorded deterministic test scope. Failing
gates: **1, 2, 6, 8**, because mandatory specification/coverage capabilities are
missing. Unverified gates: **3, 7, 9**. Human review required: **10–14**. Missing
human acceptance is never converted to PASS by this report or a helper script.

The review retained Gate 4/5 PASS only for the specified current monetary rules
and bounded synthetic reorg checks, including existing signed-spend/undo controls.
Neither PASS covers an unknown future MAINNET allocation/parameter choice, all
possible spend graphs, or operator acceptance. Any affected parameter/rule change
invalidates that evidence and requires reassessment; Gate 1 already blocks freeze.

## Review corrections before submission

The matrix incorrectly named nonexistent `src/crypto/crypto.cpp`. It now names
`src/crypto/keys.cpp`, and the checker rejects missing/unsafe implementation paths,
malformed rows and missing test references. Unapproved upgrade rules remain
explicitly NONE, not invented implementations. Empty gate requirements are rejected.
Eight standard-library unit tests cover these controls and activation refusal;
their checks remain active with Python optimization enabled. UTF-8 reads are explicit.
Only checker/tests/documentation changed during this review; prior C++ results
remain historical patch evidence until the submitted head receives its own CI.
The eight policy tests passed on Windows and Linux with and without Python
optimization; Linux's three readiness CTest entries and formatting also passed.
A broader review-phase CTest attempt stopped after starting its last entry,
without a final conclusion. WSL was subsequently observed freshly restarted;
the cause is unproven. A GitHub CLI read also reported a Windows memory allocation
failure. Both are retained separately, not relabeled as application-test failures
or as complete revalidation. Fresh submitted-head CI is still required.

Documented but NOT implemented/closed by this program: exhaustive rule coverage
(Gate 2), complete signed/retarget/cross-OS determinism (3), all requested actual
crash-point fixtures (6), exhaustive adversarial state/resource qualification (7),
missing parser fuzz targets/full library coverage (8), and complete platform
analysis (9). Gates 10–14 require actual people, operational/build evidence and
decisions; policy documents do not satisfy them. This PR is not completion of
every original TASK022 validation capability or a MAINNET readiness certificate.

| Gate | Remaining acceptance issue |
| --- | --- |
| 1 | Protocol section 4 script-limit schema does not fully match exposed parameter fields/exact P2PKH validation. MAINNET economics/genesis and upgrade compatibility are not frozen. Consensus owner must resolve the specification first. |
| 2 | The matrix maps existing assertions and new boundary tests, but is not an exhaustive audit of every normative MUST. Upgrade activation has no approved implementation/rule vectors. No skipped/missing case is counted as covered. |
| 3 | Coinbases-only deterministic corpus is not all consensus behavior. Signed graphs/retarget independent vectors and supported-OS evidence remain necessary. |
| 4 | PASS for the retained deterministic monetary tests: new signed one-unit creation, input/reward/fee overflow and issuance boundary cases supplement existing monetary tests. This is not a formal proof over all possible inputs. |
| 5 | New 1/10/100 coinbase-only reorg checks supplement signed spend/undo tests; deep signed-dependency graphs are not demonstrated by this corpus. |
| 6 | Existing WAL write/repair/truncation and restart tests do not cover every requested abrupt crash at UTXO/tip publication, wallet update and shutdown. Engineering crash fixtures remain required. |
| 7 | Existing hostile-input tests are retained; an exhaustive parser/branch-to-state/resource-invariance assessment has not been established. |
| 8 | RPC JSON/HTTP, wallet persistence and supporting-service parsers lack a complete bounded fuzz-target mapping. Smoke execution of existing targets cannot close that gap. |
| 9 | Configured strict warnings/clang-tidy, Linux ASan/UBSan CTest and TSan unit checks passed with no findings observed. The mandatory gate remains unverified as a complete platform qualification: Windows did not finish because of memory failures. No whole-program or dependency security proof is claimed. Any demonstrated consensus-related finding blocks release regardless of reviewer approval. |
| 10 | External human security audit, actual control separation, findings and independent retests are absent. Never auto-completed. |
| 11 | Accepted independent public TESTNET operations, upgrades/recovery/propagation and justified duration are absent. REGTEST and seedless shared-control evidence are not substitutes. |
| 12 | Pinned clean inputs, complete licensing/SBOM, target decisions and two actually independently controlled matching builds are absent. ALI/Maher sharing remains disclosed. |
| 13 | Provisional candidate parameters have rationales and unresolved risks, not human economic/security approval. Easy development powLimit and non-final genesis cannot become launch choices implicitly. |
| 14 | Mandatory gates are incomplete; no exact-candidate release acceptance or publication/activation scope exists. |

Security blockers: incomplete external review/parser fuzz coverage, unresolved
specification and platform qualification, actual release custody and incident intake.
Consensus blockers: unfrozen specification/upgrade policy, incomplete exhaustive
rule coverage and crash/determinism qualification. Operational blockers: independent
public TESTNET evidence, target/input licensing closure and independent builds.

## Next required work, not optional hardening

1. Consensus owner/reviewer: reconcile the specific normative schema/upgrade and
   MAINNET parameter decisions; no silent consensus change is authorized here.
2. Engineering: close the identified mandatory rule/crash/parser gaps using
   deterministic tests and reviewed minimal changes; retain failures.
3. Actual external reviewers/operators/builders: personally supply the required
   independent audit, public-testnet and reproducible-release evidence.
4. Authorized release owner: assess all exact-revision evidence and human gates.
   Acceptance of this incomplete report is not approval to unlock or launch.

No signing, tagging, package publication, infrastructure provisioning, deployment,
MAINNET activation or final-genesis creation was performed. TASK021's incomplete
operational work has not been relabeled complete by TASK022.
