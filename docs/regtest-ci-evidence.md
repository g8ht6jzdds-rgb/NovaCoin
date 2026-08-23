# Multi-process regtest CI evidence

The Unix CTest integration `nova.regtest_rpc_process` launches four independent
`novacoind` processes and one independent `nova-explorer` process. It drives
only authenticated loopback RPC/HTTP interfaces and verifies block and
transaction propagation, encrypted-wallet and journal restart recovery,
partitioning, a higher-work reorganization, mempool restoration, explorer
snapshot indexing, and invalid transaction rejection.

On Unix CTest sets `NOVACOIN_REGTEST_ARTIFACT_DIR` to the build directory. The
harness preserves each run's daemon logs, explorer log, and a `result.txt`
marker there. It does not retain RPC credentials or wallet passphrases.

GitHub Actions uses `ubuntu-24.04`, Clang/format/tidy major version 18, and
commit-pinned checkout/artifact actions. Each run retains a `build-toolchain.txt`
version manifest.
Each strict build and ASan/UBSan job uploads the CTest log, retained regtest
artifacts, CMake logs, and (for sanitizer jobs) bounded fuzz logs for 30 days.
An artifact is evidence only when its `result.txt` says `result=passed` and its
matching CTest log records `nova.regtest_rpc_process` as passed.
`scripts/verify_regtest_ci_evidence.sh` enforces both conditions before CI can
proceed to artifact upload.

This repository has no immutable Git revision or configured remote at present.
Therefore no local result can be represented as a hosted CI attestation. A
maintainer must commit these changes, push the immutable revision, and retain
the linked GitHub Actions run and artifacts before claiming the harness is
proven stable for deployment.
