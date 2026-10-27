# Building pdo_duckdb

Source builds support PHP 8.1 or newer and DuckDB 1.5.3 or newer. You need PHP's
development headers, PDO, Python 3, a C++17 compiler, the DuckDB library and
complete C header set, and the exact DuckDB source headers for that library.

The driver uses a small C++ bridge for functionality unavailable through the
stable C API. Unix configure and module initialization check DuckDB's version
and source ID. On Windows, the combined-DLL build helper executes that identity
check; configure verifies the matching cache, package identity, and bridge
export. A library upgrade requires rebuilding the bridge against its matching
source headers. Configure does not download dependencies.

## Prepare matching source headers

For the bundled DuckDB 2.0.0 release, run this from the extension checkout:

```sh
python3 scripts/duckdb-bridge-source.py stage \
  --url https://codeload.github.com/duckdb/duckdb/tar.gz/refs/tags/v2.0.0 \
  --sha256 7e89053255046bfb26afbd7f524a2fb84df7dbef496aa911ea35e4a1182fdc00 \
  --version v2.0.0 \
  --source-id d54cbbe3df17e8e5ce68312f483b4155836112aa \
  --output "$PWD/duckdb-bridge"
```

The helper verifies the archive checksum and commit metadata, stages the full
header tree, and records its identity and file hashes. To use an already
downloaded archive, replace `--url URL` with `--archive FILE` and retain the
checksum and identity arguments. To use a clean source checkout instead:

```sh
python3 scripts/duckdb-bridge-source.py stage \
  --checkout /path/to/exact-duckdb-checkout --output "$PWD/duckdb-bridge"
python3 scripts/duckdb-bridge-source.py verify "$PWD/duckdb-bridge"
```

A checkout with one exact release tag supplies its own version. For an
untagged checkout, pass its explicit `--version` and full `--source-id`.
The runtime identity check still applies; changing those arguments cannot make
headers from another engine build compatible.

The blocking compatibility builds use these source commits:

| DuckDB | Full source commit |
| --- | --- |
| 1.5.3 | `14eca11bd9d4a0de2ea0f078be588a9c1c5b279c` |
| 1.5.6 | `069cc9f9b5be802405797faecc284961b07c70ef` |
| 2.0.0 | `d54cbbe3df17e8e5ce68312f483b4155836112aa` |

## Unix shared-library build

Install the matching DuckDB release under `/opt/duckdb`, with headers in
`include/` and the shared library in `lib/`. For 2.0, install all five headers:
`duckdb.h`, `duckdb_v2.h`, `duckdb_extension.h`, `duckdb_extension_v2.h`, and
`duckdb_static_extension.h`.

```sh
phpize
./configure --with-pdo-duckdb=/opt/duckdb \
  --with-pdo-duckdb-bridge-source="$PWD/duckdb-bridge"
make -j4
make install
```

For a PIE source build, forward both paths:

```sh
pie install iliaal/pdo_duckdb --with-pdo-duckdb=/opt/duckdb \
  --with-pdo-duckdb-bridge-source=/absolute/path/to/duckdb-bridge
```

Use `--enable-pdo-duckdb-dev` to enable warnings as errors. Successful GA builds
define both `HAVE_DUCKDB_V2` and `HAVE_PDO_DUCKDB_BRIDGE` in `config.h`.
Missing, modified, or mismatched source caches fail configuration. Failed
identity checks preserve the previous successful build's library aliases.

## Self-contained Unix binaries

Extract the matching `duckdb-static-libs-*.tar.gz` asset to a flat directory and
replace the shared-library option with `--with-pdo-duckdb-static=DIR`:

```sh
./configure --with-pdo-duckdb-static=/opt/duckdb-static \
  --with-pdo-duckdb-bridge-source="$PWD/duckdb-bridge"
make -j4
```

The official GA glibc archives require GCC 13's C++ runtime. The pinned GCC 13.5
Bookworm toolchain in `scripts/Dockerfile.binary-glibc` retains the Debian 12
deployment baseline. A newer distribution's compiler can raise that baseline
even when the source is unchanged. Linux packages link the C++ runtime
statically; macOS uses the system C++ runtime and targets macOS 11.0.

The build registers the bundled static extensions before opening a database.
Archive validation disables extension downloads and exercises core functions,
JSON, Parquet reads and writes, and timezone conversion after extracting the
ZIP into a clean runtime.

## Windows source build

Build DuckDB and the bridge into one DLL with the same static MSVC runtime.
The PHP extension remains C-only and calls that DLL through the C API. This
keeps C++ allocation and destruction inside one runtime even when PHP uses
another MSVC runtime.

From an x64 MSVC developer environment, extract the GA
`duckdb-static-libs-windows-amd64.tar.gz` asset and prepare its matching source
cache. Then run:

```powershell
./scripts/build-windows-duckdb.ps1 `
  -BundleDirectory C:\build\duckdb-static `
  -BridgeSourceCache C:\build\duckdb-bridge `
  -SourceDirectory $PWD `
  -OutputDirectory C:\build\duckdb-combined
```

The helper builds and exercises the combined DLL before emitting its package
marker. Pass these options to the PHP SDK extension build:

```text
--with-pdo-duckdb=C:\build\duckdb-combined
--with-pdo-duckdb-bridge-source=C:\build\duckdb-bridge
```

If the PHP SDK does not preserve your `PATH`, set `PDO_DUCKDB_PYTHON` to the
absolute path of your Python 3 executable. Configure uses it for source-cache
and header verification. CI sets it automatically.

DuckDB 1.5 releases do not provide MSVC static archives. For those versions,
use a clean checkout of the exact source commit and its matching header cache:

```powershell
./scripts/build-windows-duckdb.ps1 `
  -EngineSourceDirectory C:\build\duckdb-source `
  -BridgeSourceCache C:\build\duckdb-bridge `
  -SourceDirectory $PWD `
  -OutputDirectory C:\build\duckdb-combined
```

This builds the engine and bridge together from source with all seven bundled
extensions. Use a fresh PHP development SDK when changing engine generations;
the SDK can retain feature definitions from a previous extension build.

Deploy the resulting extension DLL in PHP's extension directory and the
combined `duckdb.dll` beside `php.exe`. The original upstream shared DLL cannot
replace the combined DLL: it does not export the bridge entry points.

## Binary targets and verification

The binary workflows build PHP 8.2–8.5 as stable targets and the explicitly
pinned PHP 8.6.0RC4 as a development target. PHP 8.1 remains a source target.
Stable binaries require PHP 8.2.35+, 8.3.36+, 8.4.27+, or 8.5.12+ in their
respective series. They use the newer PHP allocation APIs to stop safely after
a pending memory-limit error. Older patches remain supported through source
builds; do not load these binaries on them. PIE selects archives by minor, so
use the explicit source-build command in the README for older patches.
Rebuild source-installed modules when upgrading PHP across these minimums.
The development NTS and ZTS runtimes are built separately and checked at runtime;
an image tag alone does not establish thread safety.

| Platform | Architecture | PHP threading |
| --- | --- | --- |
| Linux glibc | x86_64, arm64 | NTS, ZTS |
| Linux musl | x86_64, arm64 | NTS |
| macOS | arm64 | NTS, ZTS |
| Windows | x64 | NTS, TS |

Run `binary-validate.yml` and `binary-validate-windows.yml` through GitHub Actions
workflow dispatch to build and exercise candidate archives without publishing.
Every target checks the actual PHP ABI and loads its extracted archive before
uploading it as a workflow artifact. The release workflow requires the complete
set of 50 archive names. A matrix entry describes required coverage; its exact
workflow run establishes whether that target passed.
