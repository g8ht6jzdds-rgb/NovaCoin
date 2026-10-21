# Executable version reporting

The standalone `--version` argument on `novacoind`, `novacoin-cli`, `nova-genesis`,
`nova-explorer` and `novacoin-faucet` prints one line and exits before application
configuration, credentials, files, networking or service setup. Other invocations
retain their existing behavior; notably the CLI remains an otherwise silent stub.

Format: `<executable> <application-version> revision=<commit> tree=<tree> state=<state>`.
Version comes from CMake's project version. The build refreshes Git identity and
tracked/untracked working-tree state each time: `clean` or `dirty`. For dirty builds,
commit/tree identify HEAD, not all working-copy bytes. Build/source mutation during
compilation is unsupported: freeze inputs and independently capture hashes.
Git-ignored files, dependencies and generated outputs are outside this Git status
claim. `clean` is not a statement that every build input equals a tracked blob.

Without a Git checkout, provenance is `UNRECORDED`/`unknown`. Source-archive builds
may supply paired `NOVA_ARCHIVE_REVISION` and `NOVA_ARCHIVE_TREE` lowercase 40-hex
identifiers, reported as `archive-declared`, never verified clean provenance. These
cannot override a Git checkout. No commit timestamp, absolute path, hostname or
secret is embedded. Metadata errors fail the build rather than silently use stale
identity. Reconfigure after changing version-generation code or archive inputs.

Version text is not an SBOM, a signature, a dependency inventory or proof of
reproducibility. Package ledgers must independently record exact source bytes,
toolchain, dependencies, configuration and output hashes. Adopting a new release
input, platform support and separate reproduction remain independent decisions.

Tests check exact output, parser controls and synthetic provenance transitions.
The Linux non-sanitized subprocess guard denies selected file/network syscalls
after loader initialization and calibrates denial before testing each executable.
It is not a sandbox claim for arbitrary code or dynamic-loader activity; Windows
and sanitizer tests check output/fixture immutability without that Linux guard.
