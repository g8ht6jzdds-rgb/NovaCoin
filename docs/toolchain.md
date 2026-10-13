# Pinned development toolchain

NovaCoin's Windows development toolchain is intentionally pinned so that a
developer and CI resolve the same dependency ports and compiler family.

| Component | Pinned version | Purpose |
| --- | --- | --- |
| CMake | 4.3.3 | CMake generator and test runner |
| MSVC Build Tools | 14.44.35207 (v143), compiler 19.44.35228 | Windows C++20 ABI/reference compiler |
| LLVM | 20.1.8, installer SHA-256 `3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b` | Sanitizers, formatting, static analysis |
| vcpkg registry | `b8b8df2201ad8509b81a830fe0957bcb98e06c27` | Manifest dependency resolution |
| triplet | `x64-windows` | Debug and Release dependency ABI |

The vcpkg baseline is recorded in `vcpkg-configuration.json`. Do not update it
without recording the reason, reviewing every resolved port change, and
re-running all build and sanitizer suites.

## Configuration

Set `VCPKG_ROOT` to the vcpkg tool checkout or installation, then use
`vcpkg-debug` or `vcpkg-release`. The registry baseline above pins dependency
resolution; it is not a separate pin for the vcpkg executable/tool checkout.
Record the tool checkout's commit separately for reproduction. The non-vcpkg presets are
reserved for CI's explicitly installed system packages; they do not silently
select a dependency manager.

On Windows, use the LLVM compiler for ASan and UBSan presets. They use
`RelWithDebInfo` and the release dynamic CRT because Clang ASan cannot run with
vcpkg's Debug CRT. Use the separate `fuzz` preset for libFuzzer: it selects the
static release CRT required by LLVM's Windows libFuzzer runtime. Do not combine
unit tests and libFuzzer targets in one Windows sanitizer build.

Each Windows ASan executable stages the pinned
`clang_rt.asan_dynamic-x86_64.dll` next to itself, so CTest does not depend on
the developer's `PATH`. The LLVM installer must provide that file under the
compiler resource directory.

The local toolchain installation is not source-controlled. The reviewed
Windows installer checksums are: CMake 4.3.3
`b6c50584847f02fe7f11d94ad1d99d592b5b371c476e2de3770ae3ee823b2638`,
LLVM 20.1.8
`3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b`,
and Visual Studio Build Tools bootstrapper
`15df9d3b4c2b2eaf44704d5e938c895341b9cd8ba40a9a18610f8d18cbe01b53`.
The standalone `scripts/verify_testnet_genesis.ps1` validates the installed
toolchain identities and produces the exact testnet-genesis evidence record.

## Unresolved release-build inputs

These development pins do not define a supported release platform or a
complete reproducible-package procedure. Before packaging, record and review:

- exact OS version or build-image digest and architecture for each target;
- the Ninja version (the presets select Ninja but do not pin its version);
- the vcpkg tool commit, resolved dependency versions and port revisions,
  transitive dependencies, source hashes, and runtime-library inventory;
- Linux compiler/system-package pins and macOS compiler/SDK/Xcode pins;
- packaging-tool versions, archive layout, timestamp/path normalization,
  signing inputs, and the procedure for comparing independent build outputs.

The manifest names gtest, OpenSSL, and secp256k1; it is not an inventory of
installed dependency versions. The CMake minimum in `CMakePresets.json` is
a compatibility floor, not a replacement for the documented CMake tool pin.
Do not infer release support or reproducibility from a passing development
build or a deployment template. These release inputs remain unresolved until
platform-specific decisions and evidence are recorded.
