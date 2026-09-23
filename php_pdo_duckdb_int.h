/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2026, Ilia Alshanetsky                                 |
  | Copyright (c) 2026, Advanced Internet Designs Inc.                   |
  +----------------------------------------------------------------------+
  | This source file is subject to the BSD 3-Clause license that is      |
  | bundled with this package in the file LICENSE.                       |
  +----------------------------------------------------------------------+
  | Author: Ilia Alshanetsky <ilia@ilia.ws>                              |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_PDO_DUCKDB_INT_H
#define PHP_PDO_DUCKDB_INT_H

#include <duckdb.h>

/* Older PHP uses coarser type names in argument errors. */
#if PHP_VERSION_ID < 80300
# define zend_zval_value_name(zv) zend_zval_type_name(zv)
#endif

#if PHP_VERSION_ID < 80200
static zend_always_inline bool zend_str_has_nul_byte(const zend_string *s)
{
	return memchr(ZSTR_VAL(s), '\0', ZSTR_LEN(s)) != NULL;
}
#endif

/* Generated arginfo calls this PHP 8.4 API unconditionally. */
#if PHP_VERSION_ID < 80400
static zend_always_inline zend_class_entry *zend_register_internal_class_with_flags(
		zend_class_entry *class_entry, zend_class_entry *parent_ce, uint32_t flags)
{
	zend_class_entry *ce = zend_register_internal_class_ex(class_entry, parent_ce);
	ce->ce_flags |= flags;
	return ce;
}
#endif

/* Values must be >= PDO_ATTR_DRIVER_SPECIFIC so PDO core routes them to the
 * driver. */
#define PDO_DUCKDB_ATTR_CONFIG     (PDO_ATTR_DRIVER_SPECIFIC)      /* array, connect-time */
#define PDO_DUCKDB_ATTR_UNBUFFERED (PDO_ATTR_DRIVER_SPECIFIC + 1)  /* bool, default false */

/* errorInfo[1] codes. SQLSTATE follows the code: CONNECT is 08000, SYNTAX is
 * 42000, the rest HY000. Exec-time SQL errors such as a missing table are
 * GENERAL, not SYNTAX, because DuckDB reports them at execution. */
#define PDO_DUCKDB_ERRCODE_GENERAL   1
#define PDO_DUCKDB_ERRCODE_CONNECT   2
#define PDO_DUCKDB_ERRCODE_SYNTAX    3
#define PDO_DUCKDB_ERRCODE_SANDBOX   4
#define PDO_DUCKDB_ERRCODE_STREAMING 5

typedef struct {
	const char *file;
	int line;
	unsigned int errcode;
	char *errmsg;
} pdo_duckdb_error_info;

typedef struct {
	duckdb_database db;
	duckdb_connection conn;
	pdo_duckdb_error_info einfo;
	/* Persistent handles are escalated in place on reuse, not discarded when
	 * the request policy tightens. */
	bool external_access_disabled;
	/* PG(open_basedir) as of sandbox apply (NULL when unset). DuckDB allowlists
	 * are frozen after escalate, so a re-narrowed basedir no longer matches and
	 * enforce_sandbox fails closed. Stored as a string, not a hash, because the
	 * compare sits on the appender's per-row gate and strcmp is cheaper and
	 * collision-free. */
	char *sandbox_basedir;
	/* Mirrors dbh->is_persistent for sandbox_basedir allocation; the sandbox
	 * helpers only see H. */
	bool persistent;
	/* PDO::DUCKDB_ATTR_UNBUFFERED */
	bool unbuffered;
	/* PDO core re-applies constructor driver_options through set_attribute()
	 * after handle_factory. Accept DUCKDB_ATTR_CONFIG exactly for that reapply;
	 * later runtime calls cannot change an already-open DuckDB database. */
	bool config_reapply_pending;
} pdo_duckdb_db_handle;

typedef struct _pdo_duckdb_nested_render_type {
	duckdb_type type;
	duckdb_logical_type logical_type;
	bool owns_logical_type;
	idx_t child_count;
	struct _pdo_duckdb_nested_render_type **children;
	char **child_names;
	/* ARRAY element count, cached for the appender's per-row path (0 when not
	 * an ARRAY). */
	idx_t array_size;
} pdo_duckdb_nested_render_type;

typedef enum {
	PDO_DUCKDB_TRANSACTION_NONE,
	PDO_DUCKDB_TRANSACTION_OPEN,
	PDO_DUCKDB_TRANSACTION_CLOSE
} pdo_duckdb_transaction_effect;

typedef struct {
	pdo_duckdb_db_handle *H;
	duckdb_prepared_statement prepared;
	duckdb_result result;
	pdo_duckdb_error_info einfo;
	/* result holds a materialized rowset that must be destroyed */
	bool has_result;
	/* Forward-only, one data chunk at a time; get_col reads row `cur`. */
	duckdb_data_chunk chunk;	/* current chunk, NULL when none is loaded */
	idx_t chunk_size;			/* rows in the current chunk */
	idx_t cur;					/* current row within the chunk (valid after a fetch) */
	idx_t col_count;			/* cached result column count */
	duckdb_type *col_types;		/* cached result column type ids */
	duckdb_logical_type *col_logical_types; /* cached result logical types */
	pdo_duckdb_nested_render_type **col_nested_renderers; /* immutable type/name
									 * descriptors for direct nested rendering */
	bool started;				/* has the first chunk been fetched? */
	bool done;					/* all chunks consumed */
	/* Explicit effect for transaction control wrapped by EXPLAIN ANALYZE (whose
	 * native statement type is EXPLAIN), and for direct aliases recognized at
	 * prepare time. Applied only after native execution succeeds. */
	pdo_duckdb_transaction_effect transaction_effect;
	/* Clear DuckDB's retained bindings once per execute, including execute([]),
	 * so omitted parameters cannot reuse stale values. */
	bool binds_cleared;
} pdo_duckdb_stmt;

extern const pdo_driver_t pdo_duckdb_driver;
extern const struct pdo_stmt_methods duckdb_stmt_methods;

typedef struct {
	duckdb_appender appender;
	bool closed;
	idx_t ncols;					/* appender target column count */
	duckdb_logical_type *col_types;	/* owned; NULL if ncols == 0 */
	duckdb_type *col_type_ids;
	unsigned char *col_flags;		/* fast-path flags */
	/* Cached nested metadata; scalar leaves borrow col_types[i]. NULL if ncols == 0. */
	pdo_duckdb_nested_render_type **col_desc;
	/* CAST probes validate strings before any append. NULL entries skip probing;
	 * the array is NULL when ncols == 0. */
	duckdb_prepared_statement *col_probes;
	zend_object *pdo;	/* PDO object kept alive; it owns the connection */
	zend_object std;
} pdo_duckdb_appender;

extern zend_class_entry *pdo_duckdb_appender_ce;

static inline pdo_duckdb_appender *pdo_duckdb_appender_from_obj(zend_object *o)
{
	return (pdo_duckdb_appender *)((char *)o - offsetof(pdo_duckdb_appender, std));
}

zend_result pdo_duckdb_appender_minit(void);
const zend_function_entry *pdo_duckdb_get_driver_methods(pdo_dbh_t *dbh, int kind);

/* Frees the grow-only thread-local statement caches so they aren't reported as
 * leaks at shutdown. Safe any time; entries regrow on next use. */
void pdo_duckdb_tls_caches_shutdown(void);

/* Applies the open_basedir SQL sandbox before running SQL, covering handles
 * opened before open_basedir was tightened. Returns false if the sandbox is
 * required but could not be applied. */
bool pdo_duckdb_enforce_sandbox(pdo_duckdb_db_handle *H);

/* Keep PDO's transaction flag aligned when SQL executes transaction control
 * outside beginTransaction()/commit()/rollBack(). */
void pdo_duckdb_apply_transaction_effect(pdo_dbh_t *dbh,
	pdo_duckdb_transaction_effect effect);

/* persistent must match how errmsg was allocated (dbh vs stmt). */
void pdo_duckdb_clear_einfo(pdo_duckdb_error_info *einfo, bool persistent);

/* Records an error against the dbh, or stmt when non-NULL. msg is copied. */
extern int _pdo_duckdb_error_with_code(pdo_dbh_t *dbh, pdo_stmt_t *stmt, unsigned int code, const char *msg, const char *file, int line);
#define pdo_duckdb_error_code(dbh, code, msg) _pdo_duckdb_error_with_code(dbh, NULL, code, msg, __FILE__, __LINE__)
#define pdo_duckdb_error_stmt_code(stmt, code, msg) _pdo_duckdb_error_with_code((stmt)->dbh, stmt, code, msg, __FILE__, __LINE__)
extern int _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *msg, const char *file, int line);
#define pdo_duckdb_error(dbh, msg) _pdo_duckdb_error(dbh, NULL, msg, __FILE__, __LINE__)
#define pdo_duckdb_error_stmt(stmt, msg) _pdo_duckdb_error((stmt)->dbh, stmt, msg, __FILE__, __LINE__)

#endif /* PHP_PDO_DUCKDB_INT_H */
