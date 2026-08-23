# Testnet genesis reproduction evidence

This directory is reserved for reviewed, immutable output from two independent
executions of `scripts/verify_testnet_genesis.ps1`.

Each evidence file must name the exact committed 40-hex source revision and
must be produced by a different maintainer using a separately controlled,
pinned toolchain environment. Required names are:

```text
reproduction-a-<source-revision>.txt
reproduction-b-<source-revision>.txt
```

Do not add placeholder evidence, copied output, secrets, or private-key data.
The reviewer records the two file paths and their maintainers in
`docs/testnet-genesis-review.md`; only then may the corresponding checklist
items be changed from Pending.
