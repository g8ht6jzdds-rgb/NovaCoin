[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$SourceRevision,
    [Parameter(Mandatory = $false)]
    [string]$EvidencePath,
    [string]$BuildDirectory,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-VcpkgBaseline([string]$ConfigPath) {
    try {
        $configuration = Get-Content -LiteralPath $ConfigPath -Raw -ErrorAction Stop |
            ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "Invalid vcpkg configuration: $ConfigPath"
    }

    $registry = $configuration.'default-registry'
    if ($null -eq $registry -or $registry.kind -cne 'git' -or
        $registry.repository -cne 'https://github.com/microsoft/vcpkg') {
        throw 'Unexpected default registry configuration.'
    }

    $baseline = [string]$registry.baseline
    if ($baseline -notmatch '^[0-9a-f]{40}$') {
        throw 'vcpkg baseline must be a lowercase 40-hex git revision.'
    }
    return $baseline
}

if ($SelfTest) {
    $testRoot = Join-Path -Path ([System.IO.Path]::GetTempPath()) -ChildPath ("novacoin-genesis-verifier-selftest-" + [System.Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $testRoot | Out-Null
        $validPath = Join-Path $testRoot 'valid.json'
        @'
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "b8b8df2201ad8509b81a830fe0957bcb98e06c27"
  }
}
'@ | Set-Content -LiteralPath $validPath -NoNewline
        if ((Get-VcpkgBaseline $validPath) -ne 'b8b8df2201ad8509b81a830fe0957bcb98e06c27') {
            throw 'Current vcpkg baseline was not accepted.'
        }

        foreach ($case in @(
                '{}',
                '{"default-registry":{"kind":"git","repository":"https://github.com/microsoft/vcpkg","baseline":"not-a-revision"}}',
                '{"default-registry":{"kind":"filesystem","repository":"https://github.com/microsoft/vcpkg","baseline":"b8b8df2201ad8509b81a830fe0957bcb98e06c27"}}'
            )) {
            $invalidPath = Join-Path $testRoot ([System.Guid]::NewGuid().ToString('N') + '.json')
            Set-Content -LiteralPath $invalidPath -Value $case -NoNewline
            $rejected = $false
            try { Get-VcpkgBaseline $invalidPath | Out-Null } catch { $rejected = $true }
            if (-not $rejected) { throw 'Invalid vcpkg configuration was accepted.' }
        }
    } finally {
        if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
    }
    Write-Output 'verify_testnet_genesis configuration self-test passed'
    return
}

if ([string]::IsNullOrWhiteSpace($SourceRevision) -or [string]::IsNullOrWhiteSpace($EvidencePath)) {
    throw 'SourceRevision and EvidencePath are required unless -SelfTest is used.'
}

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
$headRevision = (& git -C $repositoryRoot rev-parse --verify HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or $headRevision.Trim().ToLowerInvariant() -ne $SourceRevision) {
    throw 'The checked-out HEAD must equal SourceRevision.'
}
$worktreeStatus = @(& git -C $repositoryRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $worktreeStatus.Count -ne 0) {
    throw 'Genesis reproduction requires a clean checkout with no untracked files.'
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

$vcpkgBaseline = Get-VcpkgBaseline (Join-Path $repositoryRoot 'vcpkg-configuration.json')

$configure = 'call "{0}" >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="{5}" -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR="{6}"' -f $vcvars, $cmake, $repositoryRoot, $BuildDirectory, $ninja, $vcpkgToolchain, $vcpkgInstalled
& cmd.exe /d /c $configure
if ($LASTEXITCODE -ne 0) { throw 'Pinned genesis configure failed.' }

$build = 'call "{0}" >nul && "{1}" --build "{2}" --target nova-genesis --parallel 2' -f $vcvars, $cmake, $BuildDirectory
& cmd.exe /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'Pinned genesis build failed.' }

$generator = Join-Path $BuildDirectory 'src/genesis/nova-genesis.exe'
if (-not (Test-Path -LiteralPath $generator)) { throw 'nova-genesis executable is missing.' }
$output = & $generator --timestamp 1704153600 --message 'NovaCoin Testnet Genesis' --target 2070ffff --reward 1000000000000000 --recipient-p2pkh 37f3432cb47a3f078ed6351c5fa25d8cfed1ad64
if ($LASTEXITCODE -ne 0) { throw 'nova-genesis failed.' }
$lines = @($output | ForEach-Object { $_.ToString() })
$evidence = @{}
foreach ($line in $lines) {
    $parts = $line.Split('=', 2)
    if ($parts.Count -eq 2) { $evidence[$parts[0]] = $parts[1] }
}
$expected = @{
    'nonce' = '2'
    'hash' = '7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c'
    'merkle_root' = '9c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a63'
    'block_hex' = '0100000000000000000000000000000000000000000000000000000000000000000000009c6f883c0ad50f42cf53c822388789052b58ed350c2b7e3b7aa4bdea2f327a6300529365ffff7020020000000101000000010000000000000000000000000000000000000000000000000000000000000000ffffffff1c005293654e6f7661436f696e20546573746e65742047656e6573697300000000010080c6a47e8d03001976a91437f3432cb47a3f078ed6351c5fa25d8cfed1ad6488ac00000000'
    'block_sha256d' = 'a44b52e7067d9724b2d2086ca5a015cf9e6f564f8a0c194c90465d40599dfc74'
    'attempts' = '3'
}
foreach ($name in $expected.Keys) {
    if ($evidence[$name] -ne $expected[$name]) {
        throw "Generated testnet genesis $name does not match the candidate commitment."
    }
}
if ($evidence.Count -ne $expected.Count) {
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
    "vcpkg_baseline=$vcpkgBaseline",
    'cmake_installer_sha256=b6c50584847f02fe7f11d94ad1d99d592b5b371c476e2de3770ae3ee823b2638',
    'llvm_installer_sha256=3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b',
    'vs_buildtools_bootstrapper_sha256=15df9d3b4c2b2eaf44704d5e938c895341b9cd8ba40a9a18610f8d18cbe01b53'
) + $lines | Set-Content -LiteralPath $EvidencePath
Write-Output "Wrote immutable reproduction evidence: $EvidencePath"
