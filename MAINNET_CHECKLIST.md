# MAINNET mandatory gates

MAINNET_LOCKED=true. **ACTIVATION FORBIDDEN.** No final genesis or unlock change.
Statuses are exact tokens; no pending gate is silently accepted. Evidence and
results are in MAINNET_READINESS_REPORT.md and docs/mainnet-readiness.md.

| Gate | Requirement | Status |
| --- | --- | --- |
| 1 | Consensus specification freeze | FAIL |
| 2 | Every consensus rule: positive/negative/boundary coverage | FAIL |
| 3 | Cross-configuration and supported-OS determinism | NOT TESTED |
| 4 | Money conservation and overflow rejection | PASS |
| 5 | 1/10/100-block reorganization safety | PASS |
| 6 | All specified database/wallet/shutdown crash points | FAIL |
| 7 | Complete hostile-input rejection/state/resource controls | NOT TESTED |
| 8 | Every externally controlled parser fuzzed with records | FAIL |
| 9 | Strict/static/ASan/UBSan/TSan findings disposed | NOT TESTED |
| 10 | External human security review | HUMAN REVIEW REQUIRED |
| 11 | Long-running independent public TESTNET | HUMAN REVIEW REQUIRED |
| 12 | Independent reproducible releases per supported target | HUMAN REVIEW REQUIRED |
| 13 | Candidate MAINNET configuration review | HUMAN REVIEW REQUIRED |
| 14 | Exact-candidate human release approval | HUMAN REVIEW REQUIRED |

No row may become PASS solely because its document exists, a reference resolves,
a single fixture passes or a different account signs. All 14 are mandatory.
Any FAIL, NOT TESTED or HUMAN REVIEW REQUIRED blocks activation. Even all PASS
does not remove the immutable startup lock or grant publication/deployment rights.

Gate 4/5 PASS records the specified monetary and bounded reorg checks on this
working patch, not formal verification, deep signed-graph coverage, operational
acceptance or safety for real value. Gate 9 retains successful Linux results but
remains unverified as a complete platform qualification; Windows did not finish.
See the exact evidence and
remaining scope limitations in MAINNET_READINESS_REPORT.md.
