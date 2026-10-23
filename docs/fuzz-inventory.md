# External parser fuzz coverage and missing targets

Gate 8 needs a current parser-to-target inventory plus ACTUAL run records. Target
existence/build success is not runtime/executions/crash/coverage evidence. Record
commit/patch, dependency/tool hashes, seed, corpus, size/RSS/time limits, run count,
exit code, crashes/sanitizer findings and measured coverage when available.

| Controlled input | Existing fuzz target | Remaining qualification |
| --- | --- | --- |
| Primitive scalars/CompactSize/lengths | fuzz_deserializer | Count/size/trailing and canonicality assertions, bounded corpus |
| Transaction bytes | fuzz_transaction_deserializer | Structural plus context-validity not implied by parsing |
| Block/header bytes | fuzz_block_deserializer; fuzz_network_message | State-invariance/ordinary validation fixtures remain necessary |
| Version/headers/inventory/addr/network lengths | fuzz_network_message | Enumerate every command/parser branch and hostile counts |
| DER/signature/public-key input | fuzz_script_signature | Cryptographic correctness and script authorization require independent vectors |
| Compact target | fuzz_compact_target | Signed/overflow/nonminimal/PoW-limit boundaries |
| Address/explorer request input | fuzz_address_parser | Does not automatically cover JSON-RPC or full HTTP grammar |
| Peer discovery | fuzz_peer_discovery | Queue/backoff/capacity state controls |
| Synchronization/admitted headers | fuzz_sync_admission | Transactional saturation/reconciliation |
| Body request retry state | fuzz_body_request_recovery | Budget/expiry/allocation failure controls |
| Bootstrap file reader | fuzz_bootstrap_reader | Byte cap not a time bound for arbitrary blocking streams |
| Offline journal/identity/replay | fuzz_offline_inspection | Canonical/corruption/report/resource/state immutability |
| RPC JSON/HTTP, encrypted wallet file, supporting-service HTTP/config parsers | NO COMPLETE TARGET MAPPING | Existing unit tests are not fuzz coverage. Inventory and add missing bounded targets before Gate 8 PASS. |

Use bounded smoke runs with fixed seed and input/RSS limits to validate targets;
real coverage-based campaigns require independently reviewed duration/corpora and
finding triage. Never claim coverage percentage from libFuzzer feature counts.
`cov` is instrumented edge coverage in that build, not percentage of consensus.
No fuzz process may start a public service, inspect live chains or use real keys.

Instrumentation limitation: existing `nova_add_fuzzer` applies libFuzzer coverage
flags to each harness translation unit. The selected library configuration adds
ASan/UBSan, but does not automatically add libFuzzer coverage instrumentation to
every separately compiled parser library. Retained `cov`/`ft` counts therefore
must not be called complete parser-library coverage. Qualify instrumentation and
corpus reachability before accepting a real coverage-guided security campaign.
