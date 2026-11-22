# Generation metadata seeds

These are local-storage metadata, not protocol vectors or usable wallet images.
The fuzz framing is one byte selecting REGTEST (even) or TESTNET (odd), one byte
giving root length, then root and descriptor bytes. The target bounds each input
to 512 bytes; metadata itself has the exact lengths in
`docs/storage-generations.md`.

All five seeds select TESTNET. Identity is ID byte `01`, little-endian network
magic `0xDAB5BFFB` from `TestnetNetworkParams`, and the exact 32 bytes parsed from
`block_hash` in `test/data/protocol/genesis.ncpv`. Prefixes are `NVROOT01` and
`NVGEN001`. Integers use Python `struct.pack('<Q', value)`, and hashes use standard
single SHA-256 (`hashlib.sha256`). No secret/signing material is present.

- `empty-testnet.bin`: sequence zero, all-zero descriptor hash, no descriptor;
  expected `Empty`.
- `present-testnet.bin`: sequence one, descriptor binds two zero-length images
  whose hashes are SHA-256 of empty bytes; expected metadata `Present` only.
- `maximum-sequence.bin`: same construction at `2^64-1`; valid metadata,
  subsequent generation preparation must fail without wrapping.
- `root-trailing.bin`: empty-root seed with root length 86 and appended `x`;
  expected `Unavailable / Malformed`.
- `descriptor-bad-hash.bin`: last descriptor byte of the sequence-one seed
  XOR 1 without updating the selector hash; expected `Unavailable / HashFailure`.

`SHA256SUMS` binds the exact five seed files. Fuzzing must copy these to a
disposable corpus directory and retain mutations/crashes there, not rewrite the
committed seeds. The target does no filesystem I/O. Bounded smoke runs do not
establish exhaustive security or storage durability.
