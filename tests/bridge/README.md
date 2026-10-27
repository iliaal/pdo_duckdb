# Exact-version bridge checks

You need the DuckDB library, the matching source commit, a C11 compiler, a C++17
compiler, and Python 3. Stage a clean source checkout before compiling:

```sh
python3 scripts/duckdb-bridge-source.py stage \
  --checkout /path/to/duckdb-source --output /path/to/header-cache
bash scripts/test-duckdb-bridge.sh /path/to/header-cache /path/to/duckdb/lib
```

For GA, pass a third argument naming an extension directory containing matching
`quack`, `httpfs`, and `legacy_parser` extensions. The query test starts and stops
its own loopback server. It does not install extensions or contact an external
server. Install these matching extensions into that directory before running the
checks; missing extensions fail the suite.

For an archive, pass `--archive FILE --sha256 SHA256 --version vX.Y.Z
--source-id FULL_COMMIT` instead of `--checkout`. Archives must contain Git commit
metadata, as official GitHub source archives do. The helper verifies the digest,
embedded commit, engine headers, required vendored fmt headers, and retained
licenses. For a clean custom
checkout without an exact release tag, supply its actual library version with
`--version`; the runtime source ID must still match that checkout's commit.

The generated `pdo_duckdb_bridge_build.h` embeds the expected engine version and
source ID. Build configuration must link and run `pdd_bridge_check()` before
enabling the bridge, and module initialization must repeat that check. A shared
library with missing C++ symbols can fail at load time before this check runs.
Exact source identity is necessary but does not replace compiler, C++ ABI, or
runtime compatibility checks.

`types.c` is a C consumer of the bridge. It compares 40,970 cells against live
DuckDB SQL casts and checks exact decimals, NUL-containing values and field names,
tuples, empty containers, aliases, empty-result metadata, and failure/SQL NULL
separation. It rejects the incomplete type returned by
`duckdb_create_logical_type(DUCKDB_TYPE_VARIANT)`, including when that type is
nested. Use a result's logical type or `pdd_bridge_type_variant()` instead.

`variant.c` checks native VARIANT traversal independently of VARCHAR rendering.
The node API preserves objects with empty or NUL-containing keys, heterogeneous
arrays, nested nulls, and exact scalar types. DuckDB's recursive conversion to a
SQL `Value` can turn empty-key objects into arrays; the bridge traverses the
original encoded nodes and converts only scalar leaves. Child handles retain
their backing storage after their parent is destroyed.

Use the explicit VARIANT object and array constructors for structured input.
They accept scalar C values or already constructed VARIANT values; SQL containers
need explicit recursive construction. A VARCHAR that resembles JSON stays a
VARCHAR. SQL STRUCT construction requires named fields, or an explicit tuple
flag for positional fields. These rules prevent silently treating an empty
object key as a tuple. Top-level null VARIANT values are SQL NULL in DuckDB;
the node API distinguishes nested VARIANT nulls from a SQL NULL root.

`query.cpp` checks parse-only preparation, affected rows, `RETURNING Count`,
transactions, lazy streams, file-writer cleanup, and GA remote execution. Batch
tests preserve successful prefixes, transaction-control progress, query-result
count suppression, and each engine's parser-error behavior. Explicit-transaction
batches retain eager parsing and last-statement counts. GA tests compare the
default parser and every legacy-parser override mode against native execution,
including statement boundaries, failed suffixes, and committed data. Diagnostic
checks preserve native text by default and JSON when `errors_as_json` is enabled.
With legacy fallback enabled, the query-specific mode probe observes which parser
accepted the SQL before PDO chooses numeric placeholder rules. It runs no SQL and
leaves syntax-error handling and transaction policy to final classification.
These tests exercise the
private bridge; PDO placeholder expansion, fetch modes,
statement attributes, and error routing need their own driver integration tests.

GA preparation checks the engine's live-result admission before local parsing.
Local and attached-database parser errors retain the engine's transaction policy.
While CONNECTed, client-side preparation errors leave the server transaction and
data untouched; execution errors follow the server policy. Matching native
prepare-error effects remotely would require a server-side prepare/validation
protocol that returns errors and transaction state without executing the SQL.
The current CONNECT path forwards remote queries, so using it during preparation
would violate the no-execution contract.

`geometry.cpp` checks PDO's uppercase hexadecimal WKB rendering at every nested
depth, including VARIANT, without changing DuckDB's quoting of other leaves.
The original renderer remains available as an independent engine oracle.
DuckDB 1.5's own canonical renderer rejects a heterogeneous VARIANT array
containing an empty-key object with `Can't cast unnamed struct to VARIANT`.
The original engine-oracle renderer preserves that explicit error. Older engines
can also silently render empty-key objects as tuples; 2.0 can render objects in
heterogeneous arrays as key/value entry lists. The PDO renderer traverses encoded
VARIANT containers on both generations, retaining object/array shape and stored
field order while using the engine's scalar formatting and quoting helpers.
Non-VARIANT canonical strings remain unchanged. The differential suite preserves
the original engine oracle and separately checks fixed VARIANT shapes, NUL bytes,
and nested SQL containers. On 1.5, alias mixed empty-key VARIANT expressions with
`AS v`: implicit expression naming itself can fail in both native query and
prepare before a result is produced.

`appender.cpp` exercises literal and quoted identifiers, GA nested schema paths,
column subsets, generated columns, sequence/default values, rollback, and
durability. `logging.cpp` checks native file-sink revocation under the caller's
instance-wide SQL serialization. Retained `duckdb_logs()` binders and concurrent
quack listener contexts cannot flush a revoked file sink after containment
returns. Existing memory logs and transaction state remain available.

`filesystem.cpp` checks shared cached opens through hardlink aliases, competing
attachments, transactions, durability, concurrent alias opens, ordinary file
I/O, and representative-path replacement refusal. Identity uses sampled local
paths; arbitrary external replacement/ABA and casefold collisions remain outside
that guarantee. Replacing a representative after `DETACH` requires closing its
filesystem owners first.
Failed attachments release only their own still-missing path references after
query cleanup. Concurrent attachments and uncertain identity samples remain
protected. A failed attachment whose path is confirmed missing cannot poison
unrelated opens.

The bridge uses only the v1 C API handle representations defined by the exact
engine source. It never converts between v1 and v2 opaque handles. Call it while
holding the driver's connection/instance lock and after its sandbox gate.
SQL NULL is a successful operation with a NULL flag or null value; errors return
`PDD_BRIDGE_ERROR`, even if allocating an error message fails. Free every owned
output before reusing its output slot. Bridge byte buffers and errors use their
matching bridge free functions. Copied C values, logical types, and chunks use
the corresponding DuckDB destroy functions and therefore require a compatible
allocation runtime.

Upstream Windows builds select a static MSVC runtime; a bridge linked to a
separate engine DLL must not
cross incompatible CRT allocation boundaries. Validate the selected combined
module or matching-runtime build on Windows before shipping it.
