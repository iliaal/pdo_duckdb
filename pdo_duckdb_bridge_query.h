#ifndef PDO_DUCKDB_BRIDGE_QUERY_H
#define PDO_DUCKDB_BRIDGE_QUERY_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdd_bridge_result pdd_bridge_result;

typedef enum {
	PDD_BRIDGE_SQL_OTHER = 0,
	PDD_BRIDGE_SQL_SELECT,
	PDD_BRIDGE_SQL_INSERT,
	PDD_BRIDGE_SQL_UPDATE,
	PDD_BRIDGE_SQL_DELETE,
	PDD_BRIDGE_SQL_MERGE,
	PDD_BRIDGE_SQL_CREATE_TABLE_AS,
	PDD_BRIDGE_SQL_COPY
} pdd_bridge_sql_kind;

typedef struct {
	pdd_bridge_sql_kind kind;
	bool has_returning;
	/* Syntactic references include DuckDB session variables; this is not a binding validator. */
	idx_t parameter_count;
} pdd_bridge_sql_info;

typedef enum {
	PDD_BRIDGE_TRANSACTION_NONE = 0,
	PDD_BRIDGE_TRANSACTION_BEGIN,
	PDD_BRIDGE_TRANSACTION_END
} pdd_bridge_transaction_effect;

typedef struct {
	idx_t rows_changed;
	idx_t completed_statements;
	pdd_bridge_transaction_effect last_transaction_effect;
	bool parse_failed;
	bool prepare_failed;
} pdd_bridge_exec_info;

/* Execute each statement once. Progress remains available on failure; only successful transaction
 * controls update last_transaction_effect. Ordinary batches preserve DuckDB's query-result count
 * suppression; force_last_count preserves PDO's explicit-transaction batch contract,
 * including the separate parse, local preparation, and execution error phases. */
pdd_bridge_status pdd_bridge_exec_batch(duckdb_connection connection, const char *sql, size_t length,
	bool force_last_count, pdd_bridge_exec_info *info, pdd_bridge_error *error);

/* Parse exactly one statement without binding or executing it. SQL must not contain NUL bytes.
 * GA refuses a live result first. Parse errors follow the local transaction's engine policy;
 * CONNECT server transactions remain untouched by client-side validation. */
pdd_bridge_status pdd_bridge_query_classify(duckdb_connection connection, const char *sql, size_t length,
	pdd_bridge_sql_info *info, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_connection_is_remote(duckdb_connection connection, bool *connected,
	pdd_bridge_error *error);
/* Instance-wide catalog state, read without executing SQL; includes uncommitted attachments. */
pdd_bridge_status pdd_bridge_connection_has_remote_catalogs(duckdb_connection connection, bool *attached,
	pdd_bridge_error *error);
/* Local grammar used by parse-only validation; reads settings without dispatching SQL. */
pdd_bridge_status pdd_bridge_connection_uses_legacy_parser(duckdb_connection connection, bool *legacy,
	pdd_bridge_error *error);
/* Resolve fallback selection for this SQL without execution or transaction invalidation. Ordinary
 * syntax errors still need final classification; a successful mode probe does not validate SQL. */
pdd_bridge_status pdd_bridge_query_uses_legacy_parser(duckdb_connection connection, const char *sql, size_t length,
	bool *legacy, pdd_bridge_error *error);
/* Stop local file writers without SQL dispatch, ending a result, or changing an existing transaction. */
pdd_bridge_status pdd_bridge_clear_local_writers(duckdb_connection connection, pdd_bridge_error *error);

/* Expand PDO bindings before calling; leave session variables for DuckDB to resolve at execution. */
/* DML completes at execute even when unbuffered is requested. */
pdd_bridge_status pdd_bridge_query_execute(duckdb_connection connection, const char *sql, size_t length,
	bool unbuffered, pdd_bridge_result **result, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_result_info(pdd_bridge_result *result, idx_t *column_count,
	idx_t *rows_changed, bool *streaming, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_result_column(pdd_bridge_result *result, idx_t column,
	pdd_bridge_bytes *name, duckdb_logical_type *type, pdd_bridge_error *error);
/* A successful NULL chunk is end-of-result. Non-NULL chunks use duckdb_destroy_data_chunk(). */
pdd_bridge_status pdd_bridge_result_fetch(pdd_bridge_result *result, duckdb_data_chunk *chunk,
	pdd_bridge_error *error);
void pdd_bridge_result_destroy(pdd_bridge_result **result);

#ifdef __cplusplus
}
#endif

#endif
