# External security audit scope — human review required

Gate 10 cannot be completed automatically. No external review is claimed here.
Reviewer identity, real administrative/recovery/build/key separation, terms,
scope dates, access and exact source tree/artifact hashes must be supplied by
the actual people. Proposed scope is not a completed audit.

| Area | Required review |
| --- | --- |
| Consensus | Normative spec completeness, all rejection/boundary cases, chainwork/ties, retarget/MTP, reorganizations and accounting; independent vectors |
| Crypto usage | libsecp256k1/OpenSSL identities, DER/low-S/hash preimages, entropy and key lifecycle; no handwritten primitives |
| Networking | Every parser/count/length/canonical check, allocation/queue/retry bounds, hostile peer state invariance and time/clock trust |
| Wallet | Signing authorization, fee/change, coin selection, encryption/storage/recovery, no private export/RPC authority over consensus |
| Storage | WAL prepare/commit, partial writes/flush/rename, tip/UTXO atomicity, crash/restart and wallet consistency |
| RPC/supporting services | Local binding/auth/role isolation, hostile JSON/HTTP, faucet/explorer/metrics leakage, secret/config custody |
| Build/release | Compiler/dependency/OS provenance, compromised inputs, CI permissions, SBOM/notices, independent reproducibility and signature/tag/publication protection |

Deliver a personally authored signed report at reviewer-accessible location with
commit/tree/spec/parameter hashes, methods, limitations, findings/severity and
reproducers. Every finding needs owner disposition, linked minimal fix, tests and
independent retest. Consensus findings block Gate 9/10/14 until resolved. State
what was NOT reviewed. Any common control disqualifying independence must remain
explicit. Audit acceptance and activation are separate human decisions.

No real-value funds/keys, live chain or public exposure required or authorized.
