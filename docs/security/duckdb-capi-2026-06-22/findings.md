# Security audit: libduckdb C API as used by pdo_duckdb

- **Date:** 2026-06-22
- **Target:** DuckDB C API (`src/main/capi/`) at `/home/ilia/sec/duckdb`, `v1.5.3-7864-gababb622f7` (post-1.5.3 dev, commit `ababb622f7`)
- **Consumer under protection:** `pdo_duckdb` (`/home/ilia/ai/pdo_duckdb`)
- **Scope:** library only, restricted to the C API surface the PDO driver actually calls. SQL engine, optimizer, storage, and DuckDB extensions are out of scope (the driver never enters them directly; it is a thin C-API consumer).
- **Method:** 4 parallel discovery subagents over the capi file groups (value/result/type/bind), recall-biased; orchestrator dataflow re-trace against the driver source for every HIGH/MED candidate.
- **Result:** **No security finding reachable from pdo_duckdb.** Every memory-safety candidate is refuted by a driver-side defense or by a library-side validation. A set of upstream-DuckDB robustness/hardening observations is recorded; none is an extension vulnerability.

## Threat model

Attacker-controlled inputs that reach the audited C API:

1. **Result data (data-out):** cell contents, LIST/MAP offset+length fields, ARRAY/STRUCT layout, ENUM physical index, validity masks, `string_t` length/pointer. Influenceable by a crafted `.duckdb` file or by query results computed over attacker data (CSV/Parquet/JSON read by DuckDB). Type metadata comes from the result schema.
2. **Bound parameters / appended data (data-in):** parameter index (user picks the position), blob/varchar bytes and lengths, appended row values, DSN path/config.

DuckDB uses libc `malloc`/`free` (not PHP's allocator); PHP `memory_limit` does not bound DuckDB-internal allocation.

## Key result: the C API pushes bounds enforcement onto the caller, and the driver does it correctly

The low-level vector/type/value accessors in the C API do **not** bounds-check indices in a release build (`-DNDEBUG`): they rely on `D_ASSERT`, which compiles to a no-op. The pdo_duckdb driver was written with this in mind and defends every attacker-reachable surface:

| C API surface | Latent footgun (release build) | Driver defense | Verdict |
|---|---|---|---|
| `duckdb_struct_type_child_name/_type`, `duckdb_struct_vector_get_child` | no bounds check on `index` | loops `i < duckdb_struct_type_child_count`; count and accessor read the **same** child vector, so they cannot disagree even on a crafted type | refuted |
| `duckdb_enum_dictionary_value` | no bound, OOB-reads validity bitmask | driver never calls it; uses `duckdb_create_enum_value(lt, idx)` which validates `idx` internally (`duckdb_value-c.cpp:585`) | refuted |
| `duckdb_union_type_member_name/_type` | accesses `child_types[index+1]`, off-by-one vs its own assert | driver guards `tag >= duckdb_union_type_member_count(lt)` before indexing (`duckdb_statement.c:428`) | refuted |
| LIST child indexing via raw `{offset,length}` | no API to validate offset/length vs child size | driver clamps `e.offset > child_size \|\| e.length > child_size - e.offset` (overflow-safe, `duckdb_statement.c:332`) | refuted |
| MAP child indexing via raw `{offset,length}` | same | driver clamps against `entries_size` (`duckdb_statement.c:393`); MAP entry struct arity (2) is structurally guaranteed by the type | refuted |
| `duckdb_create_struct_value(type, values)` reads `values[i]` for `i < child_count`, no length param | short caller array → OOB read | driver sizes `vals` to exactly `duckdb_struct_type_child_count` and only calls when all slots filled (`duckdb_appender.c:433,459`) | refuted |
| `duckdb_create_list/array/map_value` | trust caller count | driver passes explicit, correctly-derived counts | refuted |
| `duckdb_bind_*(stmt, idx, ...)` | — | library validates `idx` (1..nparams) at `prepared-c.cpp:246`; driver also derives `idx` from `paramno+1` | safe both sides |
| `duckdb_append_*` after failed `duckdb_appender_create` | `wrapper->appender` is null, `->Append` derefs null | driver checks create return code, throws + destroys, never appends on failure (`duckdb_appender.c:199`) | refuted |
| `duckdb_query`/`duckdb_get_table_names`/`duckdb_pending_prepared*` missing NULL-handle guards | null-deref | driver always passes a valid connection / a successfully-prepared statement | refuted |
| `duckdb_prepare_error` returns `Message().c_str()` | dangling if `Message()` returned by value | `ErrorData::Message()` returns `const string &` (`error_data.hpp:35`) — reference to a stored member, not a temporary | refuted |

The one nested case the driver **cannot** clamp is `DUCKDB_TYPE_ARRAY`: the C API exposes no `duckdb_array_vector_get_size`, so the driver derives the child extent as `row * array_size + i` and trusts the chunk invariant (child size == `chunk_rows * array_size`). In-bounds unless the storage/flatten layer produces an array vector whose physical child is shorter than `declared_array_size * rows`. Treated as LOW / crafted-file-only; see SS-INFO-3.

## Findings index

| ID | Title | Severity | Confidence | Reachable from pdo_duckdb | Route |
|---|---|---|---|---|---|
| SS-INFO-1 | Unchecked `malloc`+`memcpy` in stringify getters (OOM null-deref) | LOW | HIGH | yes (every cell), OOM-gated | PUBLIC-BUG / upstream hardening |
| SS-INFO-2 | `duckdb_rows_changed` type confusion on a streaming result | MEDIUM (if reached) | HIGH | no (only called on materialized result) | PUBLIC-BUG / upstream hardening |
| SS-INFO-3 | Indexed accessors lack release-build bounds checks; no `duckdb_array_vector_get_size` | LOW | HIGH | no (driver loops to matching counts) | INFORMATIONAL / upstream hardening |
| SS-INFO-4 | `duckdb_append_varchar_length` truncates 64-bit length to `uint32_t` | LOW | MEDIUM | yes (>4 GB appended string) | PUBLIC-BUG / upstream hardening |
| SS-INFO-5 | Missing NULL-handle guards on several entrypoints (asymmetry with siblings) | LOW | HIGH | no | INFORMATIONAL / upstream hardening |

All five are DuckDB-upstream robustness items, not pdo_duckdb security issues. They route to DuckDB's own bug tracker / `SECURITY.md`, not to a php-src advisory. None is attacker-controlled into a memory-corruption primitive under the extension's threat model.

---

## SS-INFO-1: Unchecked malloc + memcpy in C API stringify getters
Severity: LOW · Confidence: HIGH · Verification: SOURCE-ONLY · Route: PUBLIC-BUG / upstream hardening
Category: unchecked external-library allocation
Locations: `duckdb_value-c.cpp:326-328` (`duckdb_get_varchar`), `:290-292` (`duckdb_get_blob`), `:301-302` (`duckdb_get_bit`), `:138-139` (`duckdb_get_bignum`), `:641-643` (`duckdb_value_to_string`); `value-c.cpp:165-167` (`duckdb_value_blob`); `cast/from_decimal-c.cpp:36-38`; `include/duckdb/main/capi/cast/utils.hpp:85-87`.

Each does `p = malloc(n); memcpy(p, src, n);` with no NULL check. On allocation failure (`malloc` returns NULL — DuckDB uses libc, not PHP MM) the `memcpy` writes to NULL → SIGSEGV. `duckdb_get_varchar` is called by the driver on essentially every non-trivial result cell.

Reachability: userland-reachable but OOM-gated. Not amplifiable to OOB: `n = str.size()+1` cannot wrap for any real string, so this is a crash-on-OOM (DoS), not a heap overflow. At the memory pressure required to fail these small allocations, DuckDB's own internal allocations would typically throw first.

Impact: process crash under memory exhaustion. No control over the write target. Not a security boundary crossing.

Fix direction (upstream): NULL-check the allocation and return NULL / set the C API error, matching the rest of the C API's error contract.

## SS-INFO-2: `duckdb_rows_changed` type-confuses a streaming result
Severity: MEDIUM-if-reached · Confidence: HIGH · Verification: SOURCE-ONLY · Route: PUBLIC-BUG / upstream hardening
Category: type confusion
Location: `result-c.cpp:493-497`.

`duckdb_rows_changed` gates on `result_data.result_set_type == CAPI_RESULT_TYPE_DEPRECATED`, then `reinterpret_cast<MaterializedQueryResult &>(*result_data.result)`. The sibling `duckdb_row_count` (`:480`) instead guards `result->type == STREAM_RESULT`. If a caller invokes `duckdb_rows_changed` on a `StreamQueryResult` whose `result_set_type` is not `DEPRECATED`, the cast reads `MaterializedQueryResult` members off a `StreamQueryResult` object.

Reachability: **not reachable from pdo_duckdb.** The driver's only call site (`duckdb_driver.c:174`) passes a `duckdb_query()` result, which is always materialized, so the cast is valid there. The streaming result (`duckdb_pending_prepared_streaming`) is never passed to `duckdb_rows_changed`. The result kind is chosen by the C API caller, not by attacker data.

Fix direction (upstream): guard on `result->type == STREAM_RESULT` like `duckdb_row_count`, or assert the dynamic type before the cast.

## SS-INFO-3: Indexed accessors rely on `D_ASSERT`; no array-child size accessor
Severity: LOW · Confidence: HIGH · Verification: SOURCE-ONLY · Route: INFORMATIONAL / upstream hardening
Category: missing bounds check / missing API
Locations: `logical_types-c.cpp:332-383,246-254`; `data_chunk-c.cpp:233,236`.

`duckdb_struct_type_child_name/_type`, `duckdb_enum_dictionary_value`, `duckdb_union_type_member_name/_type`, `duckdb_struct_vector_get_child`, `duckdb_array_vector_get_child` perform no runtime index/bounds check in a release build; the only guard is `D_ASSERT` (no-op under `NDEBUG`). `duckdb_enum_dictionary_value` has no assert at all and would OOB-read the dictionary validity bitmask on a bad index. Additionally there is no `duckdb_array_vector_get_size`, so a consumer must self-derive the array child extent.

Reachability: **not reachable from pdo_duckdb.** The driver loops every indexed accessor to the matching count getter, which reads the same backing vector (see the table above), and validates the union tag. A latent footgun for any C API consumer that trusts these accessors with an externally-derived index.

Fix direction (upstream): re-check the index in the C wrapper and return NULL on out-of-range; add `duckdb_array_vector_get_size`.

## SS-INFO-4: `duckdb_append_varchar_length` truncates length to 32 bits
Severity: LOW · Confidence: MEDIUM · Verification: SOURCE-ONLY · Route: PUBLIC-BUG / upstream hardening
Category: integer truncation → data corruption
Location: `appender-c.cpp:314` (`UnsafeNumericCast<uint32_t>(length)`).

The `idx_t` (64-bit) length is cast to `uint32_t` (`UnsafeNumericCast` is a bare `static_cast` in release). Appending a string longer than `UINT32_MAX` records a truncated length in the `string_t`; the copy is bounded by the truncated value, so it is silent data corruption, not an overread. `duckdb_bind_varchar_length` uses `std::string(val, length)` with the full 64-bit length and is unaffected.

Reachability: requires appending a >4 GB string through `Pdo\Duckdb\Appender` (`duckdb_appender.c:631`). PHP can hold such strings on 64-bit, but this is an extreme edge case with no memory-safety consequence.

Fix direction (upstream): reject or error on `length > UINT32_MAX` instead of truncating.

## SS-INFO-5: Missing NULL-handle guards (sibling asymmetry)
Severity: LOW · Confidence: HIGH · Verification: SOURCE-ONLY · Route: INFORMATIONAL / upstream hardening
Category: missing NULL check
Locations: `duckdb-c.cpp:195,209` (`duckdb_query`, `duckdb_get_table_names` deref `connection`); `appender-c.cpp:180,197,217,376` (`duckdb_append_*` deref `wrapper->appender` / `value`); `pending-c.cpp:27` (`duckdb_pending_prepared*` deref `wrapper->statement`); `logical_types-c.cpp:363-371` (`duckdb_logical_type_get/set_alias` deref `type`).

These entrypoints dereference a handle without the `!handle`/`HasError()` guard that their siblings use. Reachability: **not reachable from pdo_duckdb** — the driver always passes a valid connection, a successfully-created appender, a successfully-prepared statement, and a valid logical type. Pure robustness asymmetry.

Fix direction (upstream): add the same NULL/error guards the sibling functions already have.

---

## Coverage

Files read in full: `duckdb_value-c.cpp`, `value-c.cpp`, `cast/from_decimal-c.cpp`, `cast/utils-c.cpp`, `result-c.cpp`, `data_chunk-c.cpp`, `stream-c.cpp`, `logical_types-c.cpp`, `helper-c.cpp`, `prepared-c.cpp`, `appender-c.cpp`, `pending-c.cpp`, `duckdb-c.cpp`. Driver cross-checked: `duckdb_statement.c`, `duckdb_appender.c`, `duckdb_driver.c`.

Out of scope (not entered by the driver): SQL parser/binder/optimizer/execution, storage/serialization, replacement scans, table/scalar/aggregate/copy functions, arrow interop, the deprecated row-materialization result path (`duckdb_value_*`, `duckdb_column_data` — driver uses `duckdb_fetch_chunk` + vector access instead), file_system-c, logging, threading, config_options.

Validation: SOURCE-ONLY. No PoC built (no candidate survived the dataflow gate as a reachable security bug). DuckDB build not exercised.
