[CmdletBinding(DefaultParameterSetName = 'Bundle')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Bundle')][string] $BundleDirectory,
    [Parameter(Mandatory, ParameterSetName = 'Source')][string] $EngineSourceDirectory,
    [Parameter(Mandatory)][string] $BridgeSourceCache,
    [Parameter(Mandatory)][string] $SourceDirectory,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $PythonExecutable = 'python',
    [ValidateRange(1, 4)][int] $Parallel = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fromSource = $PSCmdlet.ParameterSetName -eq 'Source'
$engine = if ($fromSource) { (Resolve-Path $EngineSourceDirectory).Path } else { $null }
$bundle = if ($fromSource) { $null } else { (Resolve-Path $BundleDirectory).Path }
$cache = (Resolve-Path $BridgeSourceCache).Path
$source = (Resolve-Path $SourceDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $output) { throw "output directory already exists: $output" }
foreach ($tool in @('cl.exe', 'link.exe', 'dumpbin.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "missing x64 MSVC tool: $tool" }
}
& $PythonExecutable (Join-Path $source 'scripts/duckdb-bridge-source.py') verify $cache
if ($LASTEXITCODE -ne 0) { throw 'bridge header cache verification failed' }
$manifest = Get-Content (Join-Path $cache 'manifest.json') -Raw | ConvertFrom-Json
if ($fromSource) {
    if ($manifest.engine_major -ne 1 -or [version]$manifest.version.TrimStart('v') -lt [version]'1.5.3') {
        throw 'the Windows source build requires DuckDB 1.5.3 or newer in the 1.x line'
    }
    foreach ($tool in @('git.exe', 'cmake.exe', 'ninja.exe')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "missing source-build tool: $tool" }
    }
    $headers = @('duckdb.h', 'duckdb_extension.h')
} else {
    if ($manifest.engine_major -ne 2) { throw 'the Windows archive build requires DuckDB 2.x static libraries' }
    $headers = @('duckdb.h', 'duckdb_v2.h', 'duckdb_extension.h', 'duckdb_extension_v2.h', 'duckdb_static_extension.h')
    foreach ($header in $headers) {
        $bundled = Get-Content (Join-Path $bundle $header) -Raw
        $matched = Get-Content (Join-Path $cache "include/$header") -Raw
        if ($bundled.Replace("`r`n", "`n") -cne $matched.Replace("`r`n", "`n")) {
            throw "static library bundle and bridge cache have different headers: $header"
        }
    }
}
$libraryNames = @('duckdb_static.lib', 'duckdb_httplib.lib', 'duckdb_loadable_extensions.lib',
    'autocomplete_extension.lib', 'core_functions_extension.lib', 'icu_extension.lib', 'json_extension.lib',
    'parquet_extension.lib', 'tpcds_extension.lib', 'tpch_extension.lib')
$libraries = if ($fromSource) { @() } else { @($libraryNames | ForEach-Object { (Resolve-Path (Join-Path $bundle $_)).Path }) }
$sources = @(Get-ChildItem $source -Filter 'pdo_duckdb_bridge*.cpp' -File | Sort-Object Name)
foreach ($required in @('pdo_duckdb_bridge.cpp', 'pdo_duckdb_bridge_query.cpp', 'pdo_duckdb_bridge_types.cpp')) {
    if ($required -notin $sources.Name) { throw "missing bridge source: $required" }
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("pdo-duckdb-link-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$success = $false

try {
    $exports = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    New-Item -ItemType Directory -Path $output, "$output/include", "$output/lib" | Out-Null
    $dll = Join-Path $output 'duckdb.dll'
    $import = Join-Path $output 'lib/duckdb.lib'
    if ($fromSource) {
        $verifiedSource = Join-Path $scratch 'source-headers'
        & $PythonExecutable (Join-Path $source 'scripts/duckdb-bridge-source.py') stage --checkout $engine --output $verifiedSource --version $manifest.version --source-id $manifest.source_id
        if ($LASTEXITCODE -ne 0) { throw 'engine source checkout identity verification failed' }
        $sourceManifest = Get-Content (Join-Path $verifiedSource 'manifest.json') -Raw | ConvertFrom-Json
        if ($sourceManifest.tree_sha256 -cne $manifest.tree_sha256) { throw 'engine source headers differ from the bridge cache' }
        $ignored = & git.exe --no-replace-objects -C $engine ls-files --others --ignored --exclude-standard
        if ($LASTEXITCODE -ne 0 -or $ignored) { throw 'engine source checkout contains ignored files that could change the build' }
        $apiHeader = Get-Content (Join-Path $cache 'include/duckdb.h') -Raw
        foreach ($declaration in [regex]::Matches($apiHeader, '(?ms)^DUCKDB_C_API\s+[^;{}]+?\b(duckdb_[A-Za-z0-9_]+)\s*\(')) {
            [void]$exports.Add($declaration.Groups[1].Value)
        }
        foreach ($header in Get-ChildItem $source -Filter 'pdo_duckdb_bridge*.h' -File) {
            foreach ($declaration in [regex]::Matches((Get-Content $header.FullName -Raw), '\b(pdd_bridge_[A-Za-z0-9_]+)\s*\(')) {
                [void]$exports.Add($declaration.Groups[1].Value)
            }
        }
        $build = Join-Path $scratch 'build'
        $shortId = $manifest.source_id.Substring(0, 10)
        $describe = "$($manifest.version)-0-g$shortId"
        & cmake.exe -G Ninja -S (Join-Path $source 'scripts/windows-duckdb-source') -B $build -DCMAKE_BUILD_TYPE=Release `
            "-DDUCKDB_ENGINE_SOURCE=$engine" "-DPDO_DUCKDB_BRIDGE_SOURCE=$source" "-DPDO_DUCKDB_BRIDGE_CACHE=$cache" `
            "-DPDO_DUCKDB_PACKAGE_OUTPUT=$output" "-DOVERRIDE_GIT_DESCRIBE=$describe" "-DGIT_COMMIT_HASH=$shortId" `
            "-DPython3_EXECUTABLE=$((Get-Command $PythonExecutable).Source)"
        if ($LASTEXITCODE -ne 0) { throw 'legacy engine and bridge CMake configuration failed' }
        & cmake.exe --build $build --target duckdb --parallel $Parallel
        if ($LASTEXITCODE -ne 0) { throw 'legacy engine and bridge build failed' }
        $head = & git.exe --no-replace-objects -C $engine rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $head -cne $manifest.source_id) { throw 'engine checkout changed during compilation' }
        $status = & git.exe --no-replace-objects -C $engine status --porcelain=v1 --untracked-files=all --ignored
        if ($LASTEXITCODE -ne 0 -or $status) { throw 'engine checkout changed during compilation' }
    } else {
        foreach ($library in $libraries) {
            & dumpbin.exe /nologo /linkermember:2 $library | ForEach-Object {
                if ($_ -match '^\s+[0-9A-Fa-f]+\s+(duckdb_[A-Za-z0-9_]+)\s*$') { [void]$exports.Add($Matches[1]) }
            }
            if ($LASTEXITCODE -ne 0) { throw "cannot inspect library: $library" }
        }
        foreach ($symbol in @('duckdb_open_ext', 'duckdb_create_instance_cache', 'duckdb_register_static_extension')) {
            if (-not $exports.Contains($symbol)) { throw "missing engine API: $symbol" }
        }
        foreach ($extension in @('autocomplete', 'core_functions', 'icu', 'json', 'parquet', 'tpcds', 'tpch', 'httplib', 'loadable_extensions')) {
            if (-not $exports.Contains("duckdb_extension_${extension}_describe")) { throw "missing static extension: $extension" }
        }

        $objects = @()
        foreach ($cpp in $sources) {
            $object = Join-Path $scratch ($cpp.BaseName + '.obj')
            & cl.exe /nologo /c /O2 /MT /EHsc /std:c++17 /bigobj /Zc:__cplusplus /DNDEBUG /DNOMINMAX /DDUCKDB_STATIC_BUILD "/I$source" "/I$cache/include" "/Fo$object" $cpp.FullName
            if ($LASTEXITCODE -ne 0) { throw "bridge compilation failed: $($cpp.Name)" }
            $objects += $object
            & dumpbin.exe /nologo /symbols $object | ForEach-Object {
                if ($_ -notmatch '\bUNDEF\b' -and $_ -match '\bExternal\s+\|\s+(pdd_bridge_[A-Za-z0-9_]+)\s*$') {
                    [void]$exports.Add($Matches[1])
                }
            }
            if ($LASTEXITCODE -ne 0) { throw "cannot inspect bridge object: $object" }
        }
        $definition = Join-Path $scratch 'duckdb.def'
        @('LIBRARY duckdb', 'EXPORTS') + @($exports | Sort-Object) | Set-Content $definition -Encoding ascii
        & link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO /OPT:REF /OPT:ICF "/OUT:$dll" "/IMPLIB:$import" "/DEF:$definition" @objects @libraries ws2_32.lib rstrtmgr.lib bcrypt.lib
        if ($LASTEXITCODE -ne 0) { throw 'combined engine and bridge link failed' }
    }
    foreach ($symbol in @('duckdb_open_ext', 'duckdb_create_instance_cache', 'pdd_bridge_check', 'pdd_bridge_error_free', 'pdd_bridge_query_execute', 'pdd_bridge_value_render')) {
        if (-not $exports.Contains($symbol)) { throw "missing engine or bridge API: $symbol" }
    }
    $actualExports = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    & dumpbin.exe /nologo /exports $dll | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+((?:duckdb_|pdd_bridge_)[A-Za-z0-9_]+)\s*$') {
            [void]$actualExports.Add($Matches[1])
        }
    }
    if ($LASTEXITCODE -ne 0) { throw 'cannot inspect combined DLL exports' }
    foreach ($symbol in $exports) {
        if (-not $actualExports.Contains($symbol)) { throw "combined DLL does not export $symbol" }
    }
    $dependencies = & dumpbin.exe /nologo /dependents $dll
    if ($LASTEXITCODE -ne 0) { throw 'cannot inspect combined DLL dependencies' }
    if ($dependencies -match '(?i)\b(?:msvcp|vcruntime|ucrtbase|api-ms-win-crt)[\w.-]*\.dll\b') {
        throw 'combined DLL has a dynamic C/C++ runtime dependency'
    }
    foreach ($header in $headers) { Copy-Item (Join-Path $cache "include/$header") "$output/include/" }
    Copy-Item (Join-Path $source 'pdo_duckdb_bridge*.h') "$output/include/"
    Copy-Item (Join-Path $cache 'include/pdo_duckdb_bridge_build.h') "$output/include/"
    Copy-Item (Join-Path $cache 'LICENSE') "$output/LICENSE.duckdb"
    Copy-Item (Join-Path $cache 'licenses/fmt/LICENSE') "$output/LICENSE.fmt"
    @('LIBRARY duckdb', 'EXPORTS') + @($actualExports | Sort-Object) | Set-Content "$output/duckdb.def" -Encoding ascii
    $probeSource = Join-Path $scratch 'bridge-probe.c'
    @'
#include "pdo_duckdb_bridge.h"
#include "pdo_duckdb_bridge_query.h"
#include "pdo_duckdb_bridge_types.h"
#include "duckdb_static_extensions.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    pdd_bridge_error error = {0};
    duckdb_config config = NULL;
    duckdb_database db = NULL;
    duckdb_connection conn = NULL;
    duckdb_result sql_result;
    pdd_bridge_result *result = NULL;
    duckdb_data_chunk chunk = NULL;
    pdd_bridge_value *value = NULL;
    pdd_bridge_bytes rendered = {0};
    bool is_null = true;
    const char *sql = "SELECT 42::INTEGER";
    int status = 1;
    if (pdd_bridge_check(&error) != PDD_BRIDGE_OK) goto cleanup;
    if (pdo_duckdb_register_static_extensions() != 0) goto cleanup;
    if (duckdb_create_config(&config) != DuckDBSuccess) goto cleanup;
    if (duckdb_set_config(config, "autoinstall_known_extensions", "false") != DuckDBSuccess ||
        duckdb_set_config(config, "autoload_known_extensions", "false") != DuckDBSuccess) goto cleanup;
    if (duckdb_open_ext(NULL, &db, config, NULL) != DuckDBSuccess) goto cleanup;
    if (duckdb_connect(db, &conn) != DuckDBSuccess) goto cleanup;
    const char *setup[] = {"LOAD autocomplete", "LOAD core_functions", "LOAD json", "LOAD icu", "LOAD parquet",
        "LOAD tpcds", "LOAD tpch", "CALL dbgen(sf=0)",
        "COPY (SELECT 42 AS n) TO 'bridge-probe.parquet' (FORMAT PARQUET)"};
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        if (duckdb_query(conn, setup[i], &sql_result) != DuckDBSuccess) {
            fprintf(stderr, "%s\n", duckdb_result_error(&sql_result));
            duckdb_destroy_result(&sql_result);
            goto cleanup;
        }
        duckdb_destroy_result(&sql_result);
    }
    if (duckdb_query(conn, "SELECT n = 42 AND json_extract('{\"n\":42}', '$.n')::INTEGER = n AND "
        "date_part('hour', timezone('America/New_York', TIMESTAMPTZ '2020-01-01 12:00:00+00')) = 7 "
        "FROM read_parquet('bridge-probe.parquet')", &sql_result) != DuckDBSuccess) {
        fprintf(stderr, "%s\n", duckdb_result_error(&sql_result));
        duckdb_destroy_result(&sql_result);
        goto cleanup;
    }
    bool capabilities = duckdb_value_boolean(&sql_result, 0, 0);
    duckdb_destroy_result(&sql_result);
    if (!capabilities) goto cleanup;
    if (pdd_bridge_query_execute(conn, sql, strlen(sql), false, &result, &error) != PDD_BRIDGE_OK) goto cleanup;
    if (pdd_bridge_result_fetch(result, &chunk, &error) != PDD_BRIDGE_OK || !chunk) goto cleanup;
    if (pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(chunk, 0), 0,
        duckdb_data_chunk_get_size(chunk), &value, &error) != PDD_BRIDGE_OK) goto cleanup;
    if (pdd_bridge_value_render(value, conn, &rendered, &is_null, &error) != PDD_BRIDGE_OK ||
        is_null || rendered.length != 2 || memcmp(rendered.data, "42", 2) != 0) goto cleanup;
    pdd_bridge_sql_info info;
    sql = "SELECT FROM";
    if (pdd_bridge_query_classify(conn, sql, strlen(sql), &info, &error) != PDD_BRIDGE_ERROR || !error.data) goto cleanup;
    pdd_bridge_error_free(&error);
    status = 0;
cleanup:
    if (error.data) { fwrite(error.data, 1, error.length, stderr); fputc('\n', stderr); }
    pdd_bridge_error_free(&error);
    pdd_bridge_bytes_free(&rendered);
    pdd_bridge_value_destroy(&value);
    duckdb_destroy_data_chunk(&chunk);
    pdd_bridge_result_destroy(&result);
    duckdb_disconnect(&conn);
    duckdb_close(&db);
    duckdb_destroy_config(&config);
    if (status == 0) puts("Combined DLL identity, extensions, bridge query/value ownership and exception handling passed");
    return status;
}
'@ | Set-Content $probeSource -Encoding ascii
    $probeExe = Join-Path $scratch 'bridge-probe.exe'
    Push-Location $scratch
    try {
        $registration = @(if ($manifest.engine_major -eq 2) { '/DHAVE_DUCKDB_STATIC_EXTENSIONS' })
        & cl.exe /nologo /TC /MD @registration "/I$output/include" "/I$source" $probeSource (Join-Path $source 'duckdb_static_extensions.c') "/Fe$probeExe" /link $import
        if ($LASTEXITCODE -ne 0) { throw 'C caller probe could not link against combined DLL' }
        Copy-Item $dll (Join-Path $scratch 'duckdb.dll')
        & $probeExe
        if ($LASTEXITCODE -ne 0) { throw 'combined DLL failed the C caller runtime probe' }
    } finally {
        Pop-Location
    }
    @('#define PDO_DUCKDB_BRIDGE_COMBINED_DLL 1', '#define PDO_DUCKDB_BRIDGE_PACKAGE_ABI 1') |
        Set-Content "$output/include/pdo_duckdb_bridge_package.h" -Encoding ascii
    Write-Host "Combined DLL exports $($actualExports.Count) engine and bridge APIs; verified identity $($manifest.version) $($manifest.source_id)"
    $success = $true
} finally {
    Remove-Item $scratch -Recurse -Force
    if (-not $success -and (Test-Path $output)) { Remove-Item $output -Recurse -Force }
}
