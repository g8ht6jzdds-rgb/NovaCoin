# Pinned development toolchain

NovaCoin's Windows development toolchain is intentionally pinned so that a
developer and CI resolve the same dependency ports and compiler family.

| Component | Pinned version | Purpose |
| --- | --- | --- |
| CMake | 4.3.3 | CMake generator and test runner |
| MSVC Build Tools | 14.44.35207 (v143), compiler 19.44.35228 | Windows C++20 ABI/reference compiler |
| LLVM | 20.1.8, installer SHA-256 `3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b` | Sanitizers, formatting, static analysis |
| vcpkg registry | `e0612b42ce44e55a0e630f2ee9d3c533a63d8bc1` | Manifest dependency resolution |
| triplet | `x64-windows` | Debug and Release dependency ABI |

The vcpkg baseline is recorded in `vcpkg-configuration.json`. Do not update it
without recording the reason, reviewing every resolved port change, and
re-running all build and sanitizer suites.

## Configuration

Set `VCPKG_ROOT` to a checkout or installed vcpkg root at the recorded
baseline, then use `vcpkg-debug` or `vcpkg-release`. The non-vcpkg presets are
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
