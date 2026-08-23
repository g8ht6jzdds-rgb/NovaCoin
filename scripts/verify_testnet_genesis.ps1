[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$SourceRevision,
    [Parameter(Mandatory = $true)]
    [string]$EvidencePath,
    [string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $PSCommandPath
$repositoryRoot = Split-Path -Parent $scriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot 'build/testnet-genesis-review'
}

$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$resolvedRevision = (& git -C $repositoryRoot rev-parse --verify "$SourceRevision^{commit}" 2>$null)
$gitExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
if ($gitExitCode -ne 0 -or $resolvedRevision.Trim().ToLowerInvariant() -ne $SourceRevision) {
    throw 'SourceRevision must name an immutable commit in this checkout.'
}

$cmake = Join-Path $repositoryRoot '.toolchain/cmake/PFiles64/CMake/bin/cmake.exe'
$ninja = Join-Path $repositoryRoot '.toolchain/vs-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$clang = Join-Path $repositoryRoot '.toolchain/llvm-20/bin/clang++.exe'
$vcvars = Join-Path $repositoryRoot '.toolchain/vs-buildtools/VC/Auxiliary/Build/vcvars64.bat'
$vcpkgToolchain = Join-Path $repositoryRoot '.toolchain/vcpkg/scripts/buildsystems/vcpkg.cmake'
$vcpkgInstalled = Join-Path $repositoryRoot 'vcpkg_installed'

foreach ($path in @($cmake, $ninja, $clang, $vcvars, $vcpkgToolchain)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required pinned toolchain component is missing: $path"
    }
}

function Assert-FileHash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Pinned installer is missing: $Path"
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected) {
        throw "Installer checksum mismatch for $Path"
    }
}

Assert-FileHash (Join-Path $repositoryRoot '.toolchain/cmake-4.3.3-windows-x86_64.msi') 'b6c50584847f02fe7f11d94ad1d99d592b5b371c476e2de3770ae3ee823b2638'
Assert-FileHash (Join-Path $repositoryRoot '.toolchain/LLVM-20.1.8-win64.exe') '3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b'
Assert-FileHash (Join-Path $repositoryRoot '.toolchain/vs_buildtools.exe') '15df9d3b4c2b2eaf44704d5e938c895341b9cd8ba40a9a18610f8d18cbe01b53'

$cmakeVersion = (& $cmake --version | Select-Object -First 1).Trim()
$clangVersion = (& $clang --version | Select-Object -First 1).Trim()
$compilerBanner = (& cmd.exe /d /c "call `"$vcvars`" >nul && cl 2>&1") -join "`n"
if ($cmakeVersion -ne 'cmake version 4.3.3' -or $clangVersion -ne 'clang version 20.1.8' -or
    $compilerBanner -notmatch 'Version 19\.44\.35228') {
    throw "Pinned compiler identity mismatch: $cmakeVersion; $clangVersion"
}

$vcpkgConfig = Get-Content -LiteralPath (Join-Path $repositoryRoot 'vcpkg-configuration.json') -Raw
if ($vcpkgConfig -notmatch 'e0612b42ce44e55a0e630f2ee9d3c533a63d8bc1') {
    throw 'Pinned vcpkg baseline is absent.'
}

$configure = 'call "{0}" >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="{5}" -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR="{6}"' -f $vcvars, $cmake, $repositoryRoot, $BuildDirectory, $ninja, $vcpkgToolchain, $vcpkgInstalled
& cmd.exe /d /c $configure
if ($LASTEXITCODE -ne 0) { throw 'Pinned genesis configure failed.' }

$build = 'call "{0}" >nul && "{1}" --build "{2}" --target nova-genesis --parallel 2' -f $vcvars, $cmake, $BuildDirectory
& cmd.exe /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'Pinned genesis build failed.' }

$generator = Join-Path $BuildDirectory 'src/genesis/nova-genesis.exe'
if (-not (Test-Path -LiteralPath $generator)) { throw 'nova-genesis executable is missing.' }
$output = & $generator --timestamp 1704153600 --message 'NovaCoin Testnet Genesis' --target 2070ffff --reward 5000000000
if ($LASTEXITCODE -ne 0) { throw 'nova-genesis failed.' }
$lines = @($output | ForEach-Object { $_.ToString() })
$evidence = @{}
foreach ($line in $lines) {
    $parts = $line.Split('=', 2)
    if ($parts.Count -eq 2) { $evidence[$parts[0]] = $parts[1] }
}
if ($evidence['hash'] -ne '25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048' -or
    $evidence['merkle_root'] -ne 'e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c' -or
    $evidence['nonce'] -ne '0' -or $evidence['attempts'] -ne '1' -or
    [string]::IsNullOrWhiteSpace($evidence['block_hex']) -or
    [string]::IsNullOrWhiteSpace($evidence['block_sha256d'])) {
    throw 'Generated testnet genesis evidence does not match the candidate commitment.'
}

$directory = Split-Path -Parent $EvidencePath
if ([string]::IsNullOrWhiteSpace($directory) -or -not (Test-Path -LiteralPath $directory)) {
    throw 'Evidence directory must already exist and be review-controlled.'
}
if (Test-Path -LiteralPath $EvidencePath) { throw 'Refusing to overwrite existing evidence.' }

@(
    'artifact=NOVACOIN-TESTNET-GENESIS-001',
    "source_revision=$SourceRevision",
    "cmake=$cmakeVersion",
    'msvc=19.44.35228',
    "clang=$clangVersion",
    'vcpkg_baseline=e0612b42ce44e55a0e630f2ee9d3c533a63d8bc1',
    'cmake_installer_sha256=b6c50584847f02fe7f11d94ad1d99d592b5b371c476e2de3770ae3ee823b2638',
    'llvm_installer_sha256=3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b',
    'vs_buildtools_bootstrapper_sha256=15df9d3b4c2b2eaf44704d5e938c895341b9cd8ba40a9a18610f8d18cbe01b53'
) + $lines | Set-Content -LiteralPath $EvidencePath
Write-Output "Wrote immutable reproduction evidence: $EvidencePath"
