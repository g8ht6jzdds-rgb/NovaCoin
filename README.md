# NovaCoin

NovaCoin is an educational, Bitcoin-inspired Layer-1 blockchain implementation
in C++20.  It is bootstrapped as build infrastructure only; no blockchain
functionality exists yet.

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
The initial CTest coverage is limited to the cryptographic foundation; no
blockchain, consensus, networking, storage, wallet, or RPC behavior exists yet.
## Local C++ dependencies

The project declares its build dependencies in `vcpkg.json`: GoogleTest,
OpenSSL, and libsecp256k1. Configure CMake with a pinned vcpkg checkout's
`scripts/buildsystems/vcpkg.cmake` toolchain file and an explicit target
triplet. Toolchain binaries and installed packages belong outside version
control (for example, in `.toolchain/`).
