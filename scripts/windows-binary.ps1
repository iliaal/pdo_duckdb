param(
    [Parameter(Mandatory)][string] $PackageVersion,
    [Parameter(Mandatory)][ValidatePattern('^8\.[1-6]$')][string] $PhpSeries,
    [Parameter(Mandatory)][ValidateSet('nts', 'ts')][string] $ThreadSafety,
    [Parameter(Mandatory)][ValidateSet('source', 'stable', 'development')][string] $Channel,
    [string] $BuildDirectory = 'builder',
    [string] $ArtifactsDirectory = 'artifacts',
    [string] $DuckDBDirectory = 'duckdb-deps',
    [string] $OutputDirectory = 'packages'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PackageVersion -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]*$') {
    throw 'unsupported package version for an archive name'
}
$expectedChannel = if ($PhpSeries -eq '8.6') { 'development' } elseif ($PhpSeries -eq '8.1') { 'source' } else { 'stable' }
if ($Channel -ne $expectedChannel) { throw 'PHP channel does not match its version' }
$vs = if ($PhpSeries -in @('8.1', '8.2', '8.3')) { 'vs16' } elseif ($PhpSeries -eq '8.6') { 'vs18' } else { 'vs17' }
$suffix = "$PhpSeries-$ThreadSafety-$vs-x86_64"
$archives = @(Get-ChildItem $ArtifactsDirectory -Filter 'php_pdo_duckdb-*.zip' -File)
if ($archives.Count -ne 1) { throw "expected one builder archive, found $($archives.Count)" }
if (-not $archives[0].BaseName.EndsWith("-$suffix", [StringComparison]::Ordinal)) {
    throw "builder archive has the wrong PHP ABI or toolchain: $($archives[0].Name)"
}

# php-windows-builder 1.9.0 retains the runtime and matching SDK under one GUID directory.
$runtimes = @(Get-ChildItem "$BuildDirectory/*/php-bin/php.exe" -File)
if ($runtimes.Count -ne 1) { throw "expected one builder PHP runtime, found $($runtimes.Count)" }
$phpDirectory = $runtimes[0].Directory.FullName
$builderRoot = Split-Path $phpDirectory
$versionHeader = Join-Path $builderRoot 'php-dev/include/main/php_version.h'
$sdkVersionLine = @(Get-Content $versionHeader | Where-Object { $_ -match '^#define PHP_VERSION "([^"]+)"$' })
if ($sdkVersionLine.Count -ne 1 -or $sdkVersionLine[0] -notmatch '^#define PHP_VERSION "([^"]+)"$') {
    throw 'cannot identify the PHP SDK version'
}
$sdkVersion = $Matches[1]
$stableVersions = @{
    '8.2' = '8.2.35'
    '8.3' = '8.3.36'
    '8.4' = '8.4.27'
    '8.5' = '8.5.12'
}
if ($Channel -eq 'stable' -and $sdkVersion -cne $stableVersions[$PhpSeries]) {
    throw "expected pinned PHP SDK $($stableVersions[$PhpSeries]), got $sdkVersion"
}
$probe = (Resolve-Path 'scripts/binary-probe.php').Path
$dependency = (Resolve-Path (Join-Path $DuckDBDirectory 'duckdb.dll')).Path
$notices = @{
    'LICENSE' = (Resolve-Path 'LICENSE').Path
    'LICENSE.duckdb' = (Resolve-Path (Join-Path $DuckDBDirectory 'LICENSE.duckdb')).Path
    'LICENSE.fmt' = (Resolve-Path (Join-Path $DuckDBDirectory 'LICENSE.fmt')).Path
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$package = Join-Path $outputPath "php_pdo_duckdb-$PackageVersion-$suffix.zip"
if (Test-Path $package) { throw "package already exists: $package" }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("pdo-duckdb-binary-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$originalPath = $env:PATH
$success = $false

try {
    $stage = Join-Path $scratch 'stage'
    Expand-Archive $archives[0].FullName -DestinationPath $stage
    $extensions = @(Get-ChildItem $stage -Filter 'php_pdo_duckdb-*.dll' -File)
    if ($extensions.Count -ne 1) { throw 'builder archive must contain exactly one extension DLL' }
    $baseName = [IO.Path]::GetFileNameWithoutExtension($package)
    Rename-Item $extensions[0].FullName "$baseName.dll"
    $symbols = @(Get-ChildItem $stage -Filter 'php_pdo_duckdb-*.pdb' -File)
    if ($symbols.Count -gt 1) { throw 'builder archive has ambiguous debug symbols' }
    if ($symbols.Count -eq 1) { Rename-Item $symbols[0].FullName "$baseName.pdb" }
    Copy-Item $dependency (Join-Path $stage 'duckdb.dll')
    foreach ($name in $notices.Keys) { Copy-Item $notices[$name] (Join-Path $stage $name) }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $package

    $unpacked = Join-Path $scratch 'unpacked'
    Expand-Archive $package -DestinationPath $unpacked
    $extension = Join-Path $unpacked "$baseName.dll"
    $packagedDependency = Join-Path $unpacked 'duckdb.dll'
    if (-not (Test-Path $extension -PathType Leaf) -or -not (Test-Path $packagedDependency -PathType Leaf)) {
        throw 'final archive is missing an extension or dependency DLL'
    }
    if ((Get-FileHash $dependency).Hash -ne (Get-FileHash $packagedDependency).Hash) {
        throw 'packaged DuckDB DLL differs from the verified engine archive'
    }
    foreach ($name in $notices.Keys) {
        if ((Get-FileHash $notices[$name]).Hash -ne (Get-FileHash (Join-Path $unpacked $name)).Hash) {
            throw "packaged notice differs from its source: $name"
        }
    }

    $runtime = Join-Path $scratch 'runtime'
    Copy-Item $phpDirectory $runtime -Recurse
    if (Test-Path (Join-Path $runtime 'duckdb.dll')) { throw 'builder runtime unexpectedly contains DuckDB' }
    Copy-Item $packagedDependency (Join-Path $runtime 'duckdb.dll')
    $php = Join-Path $runtime 'php.exe'
    $env:PATH = "$runtime;$env:SystemRoot\System32;$env:SystemRoot"
    $identityScript = Join-Path $scratch 'identity.php'
    @'
<?php
echo json_encode([PHP_VERSION, PHP_MAJOR_VERSION . '.' . PHP_MINOR_VERSION, PHP_ZTS, PHP_INT_SIZE], JSON_THROW_ON_ERROR);
'@ | Set-Content $identityScript -Encoding utf8NoBOM
    Push-Location $runtime
    try {
        $identity = & $php -n $identityScript
        if ($LASTEXITCODE -ne 0) { throw 'isolated PHP runtime failed to start' }
        $version, $series, $zts, $integerSize = $identity | ConvertFrom-Json
        if ($series -ne $PhpSeries -or [bool]$zts -ne ($ThreadSafety -eq 'ts') -or $integerSize -ne 8) {
            throw "PHP ABI mismatch: $identity"
        }
        if ($version -ne $sdkVersion) { throw "PHP runtime $version differs from SDK $sdkVersion" }
        if ($Channel -eq 'development') {
            if ($version -cne '8.6.0RC4') { throw "expected pinned development PHP 8.6.0RC4, got $version" }
        } elseif ($version -notmatch '^8\.[1-5]\.\d+$') {
            throw "expected stable PHP, got $version"
        }
        Write-Host "Validating PHP $version $ThreadSafety x64 ($Channel), SDK $sdkVersion, $vs"
        & $php -n -d "extension=$extension" $probe
        if ($LASTEXITCODE -ne 0) { throw "final archive probe failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Get-FileHash $package -Algorithm SHA256 | Format-List
    if ($env:GITHUB_OUTPUT) { "package=$package" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8 }
    $success = $true
} finally {
    $env:PATH = $originalPath
    Remove-Item $scratch -Recurse -Force
    if (-not $success -and (Test-Path $package)) { Remove-Item $package -Force }
}
