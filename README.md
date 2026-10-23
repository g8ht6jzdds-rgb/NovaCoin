# NovaCoin

NovaCoin is an educational, Bitcoin-inspired Layer-1 blockchain implementation
in C++20. Development implementations include consensus validation, chain state,
networking, wallet, RPC and tooling. Passing fixtures do not establish production
readiness; MAINNET remains locked.

## Build

Install CMake 3.25+, a C++20 compiler, Ninja, clang-format, GoogleTest,
OpenSSL 3 development files, and libsecp256k1 development files.  Use the
presets for the standard configurations:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build build/debug --target format-check
```

`release`, `asan`, and `ubsan` presets are also available.  Sanitizer presets
require Clang or GCC.

GoogleTest supplies the unit-test framework and CTest supplies test execution.
CTest covers unit, consensus and bounded loopback REGTEST integration fixtures.
See the retained readiness limitations before making platform or release claims.
## Local C++ dependencies

The project declares its build dependencies in `vcpkg.json`: GoogleTest,
OpenSSL, and libsecp256k1. Configure CMake with a pinned vcpkg checkout's
`scripts/buildsystems/vcpkg.cmake` toolchain file and an explicit target
triplet. Toolchain binaries and installed packages belong outside version
control (for example, in `.toolchain/`).

## License

NovaCoin's original project code is licensed under the [MIT License](LICENSE).
Third-party components retain their respective licenses and notice requirements.

## Versioning and release milestones

The authoritative development application version is in [VERSION](VERSION).
See [CHANGELOG.md](CHANGELOG.md), [RELEASE.md](RELEASE.md),
[versioning policy](docs/versioning.md) and the
[release checklist](docs/release-checklist.md). Milestones do not certify public
TESTNET or MAINNET readiness. MAINNET remains locked; no 1.0.0 is created.
