# Adversarial-input and fuzz readiness

Baseline: `0ecaec5ef299859cb76e90060cf17798aba6f711`. This is a bounded validation inventory, not security acceptance. MAINNET remains locked. Gate 7 and Gate 8 remain incomplete until uncovered paths, findings, and required independent review are resolved.

## Input inventory

| External input/path | Implementation | Deterministic evidence / fuzz target | Remaining limitation |
|---|---|---|---|
| P2P envelope, command, checksum, lengths, fragmented streams | `src/net/p2p.cpp`, `transport.cpp` | `P2PFraming.*`, `fuzz_network_message` | No exhaustive transport scheduling or connection exhaustion proof |
| version/verack/ping/pong/addr/getaddr/inv/getdata/getheaders/headers/tx/block | `DeserializeMessage` in `src/net/p2p.cpp` | Direct payload dispatch in `fuzz_network_message`; `P2PMessages.*` | Harness limits are test resource settings, not replacement network policy |
| CompactSize, integers, arrays, byte strings, trailing bytes | `src/primitives/serialization.cpp` | `SerializationCompactSize.*`, `SerializationMalformed.*`, `fuzz_deserializer` | Short smoke coverage is not exhaustive |
| Transactions, outpoints, scripts, blocks and headers | `src/primitives/transaction.cpp`, `block.cpp` | transaction/block suites; `fuzz_transaction_deserializer`, `fuzz_block_deserializer` | Decoder acceptance is not consensus acceptance |
| DER/public keys/signature authorization | `src/crypto`, `src/chain/validation.cpp` | `fuzz_script_signature`; protocol conformance suite | No claim of complete cryptographic coverage |
| Target encodings | `src/consensus/pow.cpp` | `fuzz_compact_target`, PoW suite | Recovery-path observational limitations remain |
| Addresses | `src/wallet/address.cpp`, explorer search | `fuzz_address_parser`, address suite | Presentation policy is not consensus |
| Peer discovery, synchronization, requested-body admission | `src/node/peer_discovery.cpp`, `sync.cpp`, `body_request_recovery.cpp` | corresponding three fuzz targets and deterministic suites | Distributed adversarial scheduling remains open |
| Bootstrap/config files and network selection | `src/node/bootstrap_reader.cpp`, `bootstrap_config.cpp`, `network_selection.cpp` | `fuzz_bootstrap_reader`, bootstrap/network-selection tests | CLI/environment parsing not completely fuzzed |
| RPC authorization and JSON bodies | `src/rpc/rpc.cpp` | `RpcIntegration.*`; `fuzz_rpc` restricted explorer principal | Administrator mutation methods and arbitrary auth strings not fully fuzzed |
| HTTP request framing | `src/rpc/http_server.cpp`, explorer/faucet HTTP servers | existing socket integration tests | Private framing functions lack direct no-socket fuzz entrypoints; not covered by RPC body fuzzing |
| Explorer response HTTP/JSON/hex | `src/explorer/rpc_snapshot_source.cpp` | explorer integration tests | Dedicated response-parser fuzz target missing |
| Faucet JSON, quota/kill-switch/audit files, RPC responses | `src/faucet` | faucet integration tests | Dedicated parser fuzz targets and exhaustive file-corruption cases missing |
| Encrypted wallet import and restored records | `src/wallet/wallet.cpp` | wallet persistence/tampering tests; `fuzz_wallet_import` | 4 KiB malformed envelope smoke does not reach authenticated plaintext or test the 64 MiB file boundary; no production wallet used |
| Block journal/database records | `src/storage/block_journal.cpp` | `JournalHostileInput.*`, storage suite, `fuzz_journal` | Explicit fuzz budgets do not establish bounded aggregate ordinary startup (which retains unlimited aggregate budgets) |
| Network identity and offline metadata/journal | `src/storage/network_identity.cpp`, `src/inspection/chain_report.cpp` | identity/inspection suites, `fuzz_offline_inspection` | Disposable synthetic evidence only; no live chain inspected |
| Executable arguments, environment and filesystem paths | daemon/CLI/genesis/inspection/explorer/faucet entrypoints | existing CLI/config tests | Full entrypoint parser inventory and direct fuzz seams remain required |

## Validation contract

`NOVA_BUILD_FUZZERS` instruments project libraries with `fuzzer-no-link,address,undefined`; each harness links libFuzzer. Verify actual compile commands before attributing coverage to production code. External crypto/system libraries are not rebuilt or instrumented by this option.

Run each target in an isolated corpus directory with seed 22, `-max_len=4096`, `-max_total_time=20`, `-timeout=2`, `-rss_limit_mb=512`, `-malloc_limit_mb=64`, and `-print_final_stats=1`. These are smoke limits, not production admission limits. Record binary/source hashes, compiler, seeds, elapsed time, executions, crashes, edge/feature counts and unavailable percentage coverage. Use UBSAN_OPTIONS=halt_on_error=1. Stop on findings and retain artifacts before any remediation.

`fuzz_wallet_import` requires `NOVA_FUZZ_TEMP` to name a dedicated empty task-owned directory. Initialization rejects a symlink directory, a nonempty directory (including a dangling input symlink), and filesystem inspection errors. It writes at most 4096 bytes and removes only its own input file. Run single-process; do not share that directory or permit concurrent writers. This contract is not isolation from a hostile local administrator. No real wallet/password/private-key input is permitted.

`fuzz_rpc` creates unpublished REGTEST state and uses public test credentials with restricted read-only authority. No listener or persistence callback exists. Its tip/address assertions are narrower than a complete UTXO checksum proof. `fuzz_journal` decodes independent in-memory streams with explicit aggregate budgets and compares output bytes for determinism.

No clean fuzz run establishes complete allocation bounds, absence of crashes on untested input, independent review, or security completion. Consensus-related sanitizer/fuzz findings block readiness. Production fixes require separately reviewed scope; never weaken an oracle to hide a finding.

## Separate release-tooling issue

At baseline `0ecaec5ef299859cb76e90060cf17798aba6f711`, the release source-archive helper inherits Git line-ending configuration. An audit export with `core.autocrlf=true` produced CRLF bytes for `.clang-format` while its committed blob contains LF. A separately named export with `core.autocrlf=false` and `core.eol=lf` matched all 293 committed blobs. The failed original archive and diagnosis remain retained. This is tracked separately in [release-tooling issue #42](https://github.com/g8ht6jzdds-rgb/NovaCoin/issues/42); this fuzz change does not fix `scripts/release.py`, rewrite archives, or treat text-normalized equality as byte equality.
