# Reproducible, non-activating release process

MAINNET_LOCKED remains true throughout TASK022. This is a process, not permission
to build/publish/sign a real-value release. No platform support is inferred from CI.

1. Owner selects an exact reviewed immutable source commit/tag, trusted public
   fingerprint and supported target list. Verify tag signature and peeled commit.
   Bind any tested dirty patch digest separately; never label it a tagged release.
2. For each target pin original OS/image identifier/provider/local hash and service
   revisions; compiler/linker/SDK/CRT, CMake/Ninja, package tools; complete direct,
   transitive, static and dynamically loaded dependency source/patch/config/hashes.
   Keep runtime prerequisites separate from redistributed payload. Record applicable
   project/dependency notices and actual acquisition/redistribution terms.
3. Define clean disposable build environment, deterministic input ordering, locale,
   timezone, timestamps, path mapping and archive metadata. No embedded secrets,
   wallets or live chain data. Do not strip/rewrite binaries after a mismatch to
   pretend repeatability. Record executable `--version` without startup side effects.
4. Two builders PERSONALLY accept the exact target and disclose real host/account/
   administrator/credential/recovery/signing boundaries. Shared ALI/Maher or other
   common effective build control is same-control repeatability, not independent.
5. Separately authorize bounded clean builds and tests from identical pinned inputs.
   Retain commands/exits, tool hashes, logs, failures and actual executable/package
   hashes. Each builder verifies source/signatures and supplies their own report.
6. Compare executable sections/PE or ELF metadata and complete unsigned artifact
   hashes. Explain nondeterministic metadata separately; investigate every mismatch.
   Two archives of one build prove only archive repeatability. Unsigned artifacts
   must match where claimed; signatures/timestamps stay outside compared payloads.
7. Produce executable version output, BUILD metadata, complete SPDX/CycloneDX SBOM
   and notices, payload inventory, sorted safe relative SHA256SUMS. Verify exact
   coverage and hashes. Signing custodian signs only accepted manifests/artifacts
   in their controlled environment; no private contents uploaded or delegated claims.
8. Separate permissions for merge, tag creation, tag push, package builds, signatures,
   publication and service activation. Verify GitHub protection MATCHES tag name;
   no bypass/force/update/delete. Publication must name exact artifact hashes,
   destination and approved scope. Audit remote identities after publication.
9. Require independent security/reproduction dispositions and all mandatory gates
   PASS for a mainnet release. Archive incident/revocation/withdrawal procedures.
   Release publication never activates services or removes MAINNET_LOCKED.

Windows current host image provenance, complete SBOM/licensing and independent
reproduction were not established by TASK021. Its local seedless signed tag and
unsigned pilot are historical evidence, not a MAINNET candidate. Linux/macOS
support requires actual target-specific pins/results, not silent exclusions.
