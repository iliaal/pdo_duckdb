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
} pdo_duckdb_db_handle;

typedef struct {
	pdo_duckdb_db_handle *H;
	duckdb_prepared_statement prepared;
	duckdb_result result;
	/* result holds a materialized rowset that must be destroyed */
	bool has_result;
	/* Result is read forward-only via the data-chunk API. We stream one chunk
	 * at a time; get_col reads column `cur` of the current chunk. */
	duckdb_data_chunk chunk;	/* current chunk, NULL when none is loaded */
	idx_t chunk_size;			/* rows in the current chunk */
	idx_t cur;					/* current row within the chunk (valid after a fetch) */
	bool started;				/* has the first chunk been fetched? */
	bool done;					/* all chunks consumed */
} pdo_duckdb_stmt;

extern const pdo_driver_t pdo_duckdb_driver;
extern const struct pdo_stmt_methods duckdb_stmt_methods;

/* Records an error against the dbh (or stmt). msg is copied; pass the message
 * obtained from duckdb_result_error()/duckdb_prepare_error() or a literal. */
extern int _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *msg, const char *file, int line);
#define pdo_duckdb_error(dbh, msg) _pdo_duckdb_error(dbh, NULL, msg, __FILE__, __LINE__)
#define pdo_duckdb_error_stmt(stmt, msg) _pdo_duckdb_error((stmt)->dbh, stmt, msg, __FILE__, __LINE__)

#endif /* PHP_PDO_DUCKDB_INT_H */
