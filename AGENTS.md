# NovaCoin engineering rules

NovaCoin is a C++20, Bitcoin-inspired educational Layer-1 project.  The
priority order is consensus correctness, deterministic behavior, security,
testability, simple architecture, maintainability, then performance.

## Consensus rules

- Never silently invent consensus behavior; record it in `docs/protocol.md` first.
- Put every consensus-critical constant in explicit immutable network parameters.
- Keep consensus independent of networking, RPC, wallet, UI, and storage.
- Use fixed-width integers for money and exact integer arithmetic for difficulty.
- Never use floating point in consensus-critical calculations.
- Require canonical serialization and reject malformed, ambiguous, noncanonical,
  oversized, and overflowing network input.
- A locally mined or received block must traverse the same validator.
- The wallet must never decide consensus validity.
- Do not enable MAINNET without an explicit documented decision.

## Engineering and testing rules

- Treat all network input as hostile; bound payloads, collection sizes, and allocations.
- Prefer RAII, standard-library value types, and maintained cryptographic libraries.
- Avoid raw owning pointers and handwritten cryptographic primitives.
- Keep changes small and scoped; do not rewrite unrelated code.
- Add deterministic positive and negative tests for every consensus rule.
- Add deterministic fuzz targets for parsers and consensus deserializers.
- Run the relevant build, tests, formatting, and static analysis before handoff.
- Passing tests do not make this project production-ready.
