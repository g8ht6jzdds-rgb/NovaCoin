# NovaCoin AI-assisted engineering workflow

NovaCoin is a C++20, Bitcoin-inspired educational Layer-1 project. The
priority order is consensus correctness, deterministic behavior, security,
testability, simple architecture, maintainability, then performance. Passing
automated checks never establishes production or MAINNET readiness.

## Required task sequence

Every engineering task follows this sequence. State when a step is not
applicable; do not silently skip it.

## Canonical module-task prompt

For every module-specific task, start from this prompt and replace
`<number>` with the assigned task number:

```text
Implement TASK <number> according to AGENTS.md.

Before changing code:

inspect the relevant repository files and protocol specification.

After implementation:

run all applicable tests,
review your diff,
and report remaining risks.

Do not modify unrelated code.
```

### 1. Inspect

- Read the relevant implementation, tests, and protocol documentation before
  proposing or making a change.
- Never assume that an API, validation path, or consensus rule exists without
  inspecting the current source and tests.
- Identify the source of every value or behavior relied upon by the change.

### 2. Define

Before coding, state:

- the goal;
- affected modules;
- consensus impact;
- security impact; and
- files expected to change.

If the expected scope changes materially, stop and update the definition before
continuing.

### 3. Implement minimally

- Implement only the requested coherent change.
- Keep changes small and scoped; do not mix unrelated refactoring with feature,
  consensus, security, or test work.
- Preserve existing behavior outside the explicitly defined scope.

### 4. Test locally

Add deterministic positive, negative, and boundary tests whenever the change
can affect behavior. For documentation-only work, use a deterministic binding
or drift check when practical and state why any test category does not apply.

### 5. Build

Compile every affected target. If no build target is affected, report that
explicitly rather than claiming a build passed.

### 6. Validate

Run the relevant unit, integration, and consensus tests, plus applicable static
analysis and sanitizers. Preserve failed and unavailable checks; never relabel
an unavailable check as a pass.

### 7. Review the diff

Before handoff, inspect the final Git diff and status. Specifically check for:

- unexpected files or generated artifacts;
- consensus behavior changes;
- removed or weakened validation;
- unchecked integer arithmetic or unsafe casts;
- memory-lifetime errors;
- private-key exposure or secret files;
- unintended RPC exposure;
- unbounded network-payload allocation; and
- nondeterministic behavior.

### 8. Report

Every handoff reports:

- summary;
- changed files;
- tests executed and their outcomes;
- new assumptions;
- remaining risks; and
- recommended next task.

## Consensus rules

- Never silently invent consensus behavior; record it in `docs/protocol.md`
  first.
- Put every consensus-critical constant in explicit immutable network
  parameters.
- Keep consensus independent of networking, RPC, wallet, UI, and storage.
- Use fixed-width integers for money and exact integer arithmetic for
  difficulty. Never use floating point in consensus-critical calculations.
- Require canonical serialization and reject malformed, ambiguous,
  noncanonical, oversized, and overflowing network input.
- A locally mined or received block must traverse the same validator.
- The wallet must never decide consensus validity.
- Do not enable MAINNET without an explicit documented decision.

## Consensus-change procedure

If a requested change might affect consensus, stop the normal implementation
flow before coding. First create a **CONSENSUS CHANGE ANALYSIS** that records:

1. old behavior;
2. proposed behavior;
3. affected block and transaction validity;
4. backward compatibility;
5. fork risk;
6. activation requirements; and
7. TESTNET consequences.

Only then may the change proceed through the ordinary task sequence. Do not
change consensus rules merely to make a test pass, and do not change
serialization or chain selection without compatibility analysis and dedicated
fork/reorganization tests.

## Security and hostile-input rules

Treat input from P2P, RPC, wallet imports, files, databases, and command-line
arguments as untrusted until it has been validated.

- Treat all network input as hostile; bound payloads, collection sizes, and
  allocations.
- Never trust peer-provided values or deserialize unbounded network arrays.
- Never log wallet private keys, store secret keys in source control, or
  hard-code release private keys.
- Do not expose RPC publicly by default.
- Prefer RAII, standard-library value types, and maintained cryptographic
  libraries. Avoid raw owning pointers and handwritten cryptographic
  primitives.
- Do not weaken a test simply because the implementation fails.

## Testing and validation rules

- Add deterministic positive and negative tests for every consensus rule, and
  include a boundary test where the rule has a meaningful boundary.
- Add deterministic fuzz targets for parsers and consensus deserializers.
- Run relevant build, tests, formatting, and static analysis before handoff.
- Treat consensus-related static-analysis or sanitizer findings as blockers
  until explicitly investigated and resolved through the documented process.

## Commits

Use one logically coherent change per task where practical. Suggested commit
prefixes are:

`core:`, `consensus:`, `net:`, `wallet:`, `rpc:`, `storage:`, `test:`,
`docs:`, `build:`, and `security:`.

Example: `consensus: reject block subsidy overflow`.

## Review passes

After every major milestone, schedule a review-only Codex task. The reviewer
must not implement features in that review. Review at least:

- correctness;
- consensus;
- security;
- performance;
- maintainability; and
- testing.

Never claim that a milestone is secure merely because automated tests pass.
