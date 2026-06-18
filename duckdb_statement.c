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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"

static void pdo_duckdb_stmt_reset_result(pdo_duckdb_stmt *S)
{
	if (S->has_result) {
		duckdb_destroy_result(&S->result);
		S->has_result = false;
	}
	S->current_row = -1;
	S->row_count = 0;
}

static int pdo_duckdb_stmt_dtor(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	pdo_duckdb_stmt_reset_result(S);
	if (S->prepared) {
		duckdb_destroy_prepare(&S->prepared);
		S->prepared = NULL;
	}
	efree(S);
	return 1;
}

static int pdo_duckdb_stmt_execute(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	/* discard any rowset from a previous execution */
	pdo_duckdb_stmt_reset_result(S);

	if (duckdb_execute_prepared(S->prepared, &S->result) != DuckDBSuccess) {
		pdo_duckdb_error_stmt(stmt, duckdb_result_error(&S->result));
		duckdb_destroy_result(&S->result);
		return 0;
	}

	S->has_result = true;
	S->current_row = -1;
	S->row_count = (int64_t)duckdb_row_count(&S->result);

	php_pdo_stmt_set_column_count(stmt, (int)duckdb_column_count(&S->result));
	stmt->row_count = (zend_long)duckdb_rows_changed(&S->result);

	return 1;
}

static int pdo_duckdb_stmt_param_hook(pdo_stmt_t *stmt, struct pdo_bound_param_data *param,
		enum pdo_param_event event_type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	zval *parameter;
	idx_t idx;

	if (!param->is_param) {
		return 1;
	}

	/* Resolve the rewritten "$N" placeholder name into a 0-based paramno.
	 * Positional binds arrive with name == NULL and paramno already set. */
	if (event_type == PDO_PARAM_EVT_NORMALIZE) {
		if (param->name) {
			if (ZSTR_VAL(param->name)[0] == '$') {
				param->paramno = ZEND_ATOL(ZSTR_VAL(param->name) + 1) - 1;
			} else if (stmt->bound_param_map) {
				zend_string *nv = zend_hash_find_ptr(stmt->bound_param_map, param->name);
				if (nv == NULL) {
					pdo_duckdb_error_stmt(stmt, "parameter was not defined");
					return 0;
				}
				param->paramno = ZEND_ATOL(ZSTR_VAL(nv) + 1) - 1;
			}
		}
		return 1;
	}

	if (event_type != PDO_PARAM_EVT_EXEC_PRE) {
		return 1;
	}

	if (param->paramno < 0) {
		pdo_duckdb_error_stmt(stmt, "Cannot bind a parameter without a position");
		return 0;
	}
	idx = (idx_t)(param->paramno + 1);

	if (Z_ISREF(param->parameter)) {
		parameter = Z_REFVAL(param->parameter);
	} else {
		parameter = &param->parameter;
	}

	switch (PDO_PARAM_TYPE(param->param_type)) {
		case PDO_PARAM_STMT:
			return 0;

		case PDO_PARAM_NULL:
			if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
				return 1;
			}
			pdo_duckdb_error_stmt(stmt, "Failed to bind NULL parameter");
			return 0;

		case PDO_PARAM_INT:
		case PDO_PARAM_BOOL:
			if (Z_TYPE_P(parameter) == IS_NULL) {
				if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
					return 1;
				}
			} else if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_BOOL) {
				if (duckdb_bind_boolean(S->prepared, idx, zend_is_true(parameter)) == DuckDBSuccess) {
					return 1;
				}
			} else {
				convert_to_long(parameter);
				if (duckdb_bind_int64(S->prepared, idx, (int64_t)Z_LVAL_P(parameter)) == DuckDBSuccess) {
					return 1;
				}
			}
			pdo_duckdb_error_stmt(stmt, "Failed to bind integer parameter");
			return 0;

		case PDO_PARAM_LOB:
			if (Z_TYPE_P(parameter) == IS_RESOURCE) {
				php_stream *stm = NULL;
				php_stream_from_zval_no_verify(stm, parameter);
				if (stm) {
					zend_string *mem = php_stream_copy_to_mem(stm, PHP_STREAM_COPY_ALL, 0);
					zval_ptr_dtor(parameter);
					ZVAL_STR(parameter, mem ? mem : ZSTR_EMPTY_ALLOC());
				} else {
					pdo_raise_impl_error(stmt->dbh, stmt, "HY105", "Expected a stream resource");
					return 0;
				}
			} else if (Z_TYPE_P(parameter) == IS_NULL) {
				if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
					return 1;
				}
				pdo_duckdb_error_stmt(stmt, "Failed to bind NULL parameter");
				return 0;
			} else if (!try_convert_to_string(parameter)) {
				return 0;
			}

			if (duckdb_bind_blob(S->prepared, idx, Z_STRVAL_P(parameter), (idx_t)Z_STRLEN_P(parameter)) == DuckDBSuccess) {
				return 1;
			}
			pdo_duckdb_error_stmt(stmt, "Failed to bind blob parameter");
			return 0;

		case PDO_PARAM_STR:
		default:
			if (Z_TYPE_P(parameter) == IS_NULL) {
				if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
					return 1;
				}
			} else {
				if (!try_convert_to_string(parameter)) {
					return 0;
				}
				if (duckdb_bind_varchar_length(S->prepared, idx, Z_STRVAL_P(parameter), (idx_t)Z_STRLEN_P(parameter)) == DuckDBSuccess) {
					return 1;
				}
			}
			pdo_duckdb_error_stmt(stmt, "Failed to bind string parameter");
			return 0;
	}
}

static int pdo_duckdb_stmt_fetch(pdo_stmt_t *stmt,
	enum pdo_fetch_orientation ori, zend_long offset)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (!S->has_result) {
		return 0;
	}

	/* forward-only cursor */
	if (ori != PDO_FETCH_ORI_NEXT) {
		pdo_duckdb_error_stmt(stmt, "DuckDB PDO driver only supports forward-only cursors");
		return 0;
	}

	if (S->current_row + 1 >= S->row_count) {
		S->current_row = S->row_count;
		return 0;
	}

	S->current_row++;
	return 1;
}

static int pdo_duckdb_stmt_describe(pdo_stmt_t *stmt, int colno)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	const char *name;

	if (!S->has_result || (idx_t)colno >= duckdb_column_count(&S->result)) {
		return 0;
	}

	name = duckdb_column_name(&S->result, (idx_t)colno);
	stmt->columns[colno].name = zend_string_init(name ? name : "", name ? strlen(name) : 0, 0);
	stmt->columns[colno].maxlen = SIZE_MAX;
	stmt->columns[colno].precision = 0;

	return 1;
}

static int pdo_duckdb_stmt_get_col(
		pdo_stmt_t *stmt, int colno, zval *result, enum pdo_param_type *type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	idx_t col = (idx_t)colno;
	idx_t row;

	if (!S->has_result || S->current_row < 0 || S->current_row >= S->row_count) {
		return 0;
	}
	if (col >= duckdb_column_count(&S->result)) {
		return 0;
	}
	row = (idx_t)S->current_row;

	if (duckdb_value_is_null(&S->result, col, row)) {
		ZVAL_NULL(result);
		return 1;
	}

	switch (duckdb_column_type(&S->result, col)) {
		case DUCKDB_TYPE_BOOLEAN:
			ZVAL_LONG(result, duckdb_value_boolean(&S->result, col, row) ? 1 : 0);
			return 1;

		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER: {
			/* These all fit in a signed 64-bit zend_long. UBIGINT/HUGEINT may
			 * not, so they fall through to the varchar path below. */
			ZVAL_LONG(result, (zend_long)duckdb_value_int64(&S->result, col, row));
			return 1;
		}

		case DUCKDB_TYPE_FLOAT:
		case DUCKDB_TYPE_DOUBLE:
			ZVAL_DOUBLE(result, duckdb_value_double(&S->result, col, row));
			return 1;

		case DUCKDB_TYPE_BLOB: {
			duckdb_blob blob = duckdb_value_blob(&S->result, col, row);
			if (blob.data) {
				ZVAL_STRINGL(result, (char *)blob.data, (size_t)blob.size);
				duckdb_free(blob.data);
			} else {
				ZVAL_EMPTY_STRING(result);
			}
			return 1;
		}

		default: {
			/* VARCHAR and every type without a dedicated native mapping
			 * (DATE/TIME/TIMESTAMP/DECIMAL/HUGEINT/UBIGINT/INTERVAL/nested...)
			 * are returned as their canonical string form. */
			char *str = duckdb_value_varchar(&S->result, col, row);
			if (str) {
				ZVAL_STRING(result, str);
				duckdb_free(str);
			} else {
				ZVAL_NULL(result);
			}
			return 1;
		}
	}
}

static int pdo_duckdb_stmt_col_meta(pdo_stmt_t *stmt, zend_long colno, zval *return_value)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (!S->has_result || (idx_t)colno >= duckdb_column_count(&S->result)) {
		return FAILURE;
	}

	array_init(return_value);

	switch (duckdb_column_type(&S->result, (idx_t)colno)) {
		case DUCKDB_TYPE_BOOLEAN:
			add_assoc_string(return_value, "native_type", "BOOLEAN");
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_BOOL);
			break;

		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER:
			add_assoc_string(return_value, "native_type", "INTEGER");
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_INT);
			break;

		case DUCKDB_TYPE_FLOAT:
		case DUCKDB_TYPE_DOUBLE:
			add_assoc_string(return_value, "native_type", "DOUBLE");
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_STR);
			break;

		case DUCKDB_TYPE_BLOB:
			add_assoc_string(return_value, "native_type", "BLOB");
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_LOB);
			break;

		default:
			add_assoc_string(return_value, "native_type", "VARCHAR");
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_STR);
			break;
	}

	return SUCCESS;
}

const struct pdo_stmt_methods duckdb_stmt_methods = {
	.dtor = pdo_duckdb_stmt_dtor,
	.executer = pdo_duckdb_stmt_execute,
	.fetcher = pdo_duckdb_stmt_fetch,
	.describer = pdo_duckdb_stmt_describe,
	.get_col = pdo_duckdb_stmt_get_col,
	.param_hook = pdo_duckdb_stmt_param_hook,
	.get_column_meta = pdo_duckdb_stmt_col_meta,
};
