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
	if (S->chunk) {
		duckdb_destroy_data_chunk(&S->chunk);
		S->chunk = NULL;
	}
	if (S->has_result) {
		duckdb_destroy_result(&S->result);
		S->has_result = false;
	}
	S->chunk_size = 0;
	S->cur = 0;
	S->started = false;
	S->done = false;
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

	pdo_duckdb_stmt_reset_result(S);

	if (duckdb_execute_prepared(S->prepared, &S->result) != DuckDBSuccess) {
		pdo_duckdb_error_stmt(stmt, duckdb_result_error(&S->result));
		duckdb_destroy_result(&S->result);
		return 0;
	}

	S->has_result = true;
	php_pdo_stmt_set_column_count(stmt, (int)duckdb_column_count(&S->result));
	stmt->row_count = (zend_long)duckdb_rows_changed(&S->result);

	return 1;
}

/* {{{ value reconstruction
 *
 * The legacy duckdb_value_* row API cannot materialize newer/nested types
 * (UUID, TIMESTAMPTZ, ENUM, BIT, LIST, STRUCT, MAP, ...): it returns NULL.
 * The data-chunk API has no per-cell stringifier either, but it does let us
 * rebuild a duckdb_value from the vector and hand that to duckdb_get_varchar(),
 * which renders any type — including nested ones — to its canonical text.
 * Native scalar types are taken directly into a zval in get_col; the
 * reconstruction path below is only used for the "render as string" types and
 * for recursing into nested children. */

static duckdb_value pdo_duckdb_cell_to_value(duckdb_vector vec, idx_t row);

static duckdb_value pdo_duckdb_decimal_value(duckdb_logical_type lt, void *data, idx_t row)
{
	duckdb_decimal d;
	d.width = duckdb_decimal_width(lt);
	d.scale = duckdb_decimal_scale(lt);

	switch (duckdb_decimal_internal_type(lt)) {
		case DUCKDB_TYPE_SMALLINT: {
			int64_t v = ((int16_t *)data)[row];
			d.value.lower = (uint64_t)v;
			d.value.upper = v < 0 ? -1 : 0;
			break;
		}
		case DUCKDB_TYPE_INTEGER: {
			int64_t v = ((int32_t *)data)[row];
			d.value.lower = (uint64_t)v;
			d.value.upper = v < 0 ? -1 : 0;
			break;
		}
		case DUCKDB_TYPE_BIGINT: {
			int64_t v = ((int64_t *)data)[row];
			d.value.lower = (uint64_t)v;
			d.value.upper = v < 0 ? -1 : 0;
			break;
		}
		case DUCKDB_TYPE_HUGEINT:
		default:
			d.value = ((duckdb_hugeint *)data)[row];
			break;
	}
	return duckdb_create_decimal(d);
}

static duckdb_value pdo_duckdb_enum_value(duckdb_logical_type lt, void *data, idx_t row)
{
	uint64_t idx;
	switch (duckdb_enum_internal_type(lt)) {
		case DUCKDB_TYPE_USMALLINT: idx = ((uint16_t *)data)[row]; break;
		case DUCKDB_TYPE_UINTEGER:  idx = ((uint32_t *)data)[row]; break;
		case DUCKDB_TYPE_UTINYINT:
		default:                    idx = ((uint8_t *)data)[row];  break;
	}
	return duckdb_create_enum_value(lt, idx);
}

static duckdb_value pdo_duckdb_cell_to_value(duckdb_vector vec, idx_t row)
{
	duckdb_logical_type lt = duckdb_vector_get_column_type(vec);
	duckdb_type tid = duckdb_get_type_id(lt);
	uint64_t *validity = duckdb_vector_get_validity(vec);
	void *data;
	duckdb_value ret;

	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		duckdb_destroy_logical_type(&lt);
		return duckdb_create_null_value();
	}

	data = duckdb_vector_get_data(vec);

	switch (tid) {
		case DUCKDB_TYPE_BOOLEAN:   ret = duckdb_create_bool(((bool *)data)[row]); break;
		case DUCKDB_TYPE_TINYINT:   ret = duckdb_create_int8(((int8_t *)data)[row]); break;
		case DUCKDB_TYPE_SMALLINT:  ret = duckdb_create_int16(((int16_t *)data)[row]); break;
		case DUCKDB_TYPE_INTEGER:   ret = duckdb_create_int32(((int32_t *)data)[row]); break;
		case DUCKDB_TYPE_BIGINT:    ret = duckdb_create_int64(((int64_t *)data)[row]); break;
		case DUCKDB_TYPE_UTINYINT:  ret = duckdb_create_uint8(((uint8_t *)data)[row]); break;
		case DUCKDB_TYPE_USMALLINT: ret = duckdb_create_uint16(((uint16_t *)data)[row]); break;
		case DUCKDB_TYPE_UINTEGER:  ret = duckdb_create_uint32(((uint32_t *)data)[row]); break;
		case DUCKDB_TYPE_UBIGINT:   ret = duckdb_create_uint64(((uint64_t *)data)[row]); break;
		case DUCKDB_TYPE_HUGEINT:   ret = duckdb_create_hugeint(((duckdb_hugeint *)data)[row]); break;
		case DUCKDB_TYPE_UHUGEINT:  ret = duckdb_create_uhugeint(((duckdb_uhugeint *)data)[row]); break;
		case DUCKDB_TYPE_FLOAT:     ret = duckdb_create_float(((float *)data)[row]); break;
		case DUCKDB_TYPE_DOUBLE:    ret = duckdb_create_double(((double *)data)[row]); break;
		case DUCKDB_TYPE_DATE:      ret = duckdb_create_date(((duckdb_date *)data)[row]); break;
		case DUCKDB_TYPE_TIME:      ret = duckdb_create_time(((duckdb_time *)data)[row]); break;
		case DUCKDB_TYPE_TIME_TZ:   ret = duckdb_create_time_tz_value(((duckdb_time_tz *)data)[row]); break;
		case DUCKDB_TYPE_TIMESTAMP: ret = duckdb_create_timestamp(((duckdb_timestamp *)data)[row]); break;
		case DUCKDB_TYPE_TIMESTAMP_TZ: ret = duckdb_create_timestamp_tz(((duckdb_timestamp *)data)[row]); break;
		case DUCKDB_TYPE_INTERVAL:  ret = duckdb_create_interval(((duckdb_interval *)data)[row]); break;
		case DUCKDB_TYPE_UUID: {
			/* DuckDB stores UUIDs as int128 with the sign bit flipped so they
			 * sort correctly; duckdb_create_uuid() wants the logical value. */
			duckdb_uhugeint u = ((duckdb_uhugeint *)data)[row];
			u.upper ^= ((uint64_t)1 << 63);
			ret = duckdb_create_uuid(u);
			break;
		}
		case DUCKDB_TYPE_DECIMAL:   ret = pdo_duckdb_decimal_value(lt, data, row); break;
		case DUCKDB_TYPE_ENUM:      ret = pdo_duckdb_enum_value(lt, data, row); break;

		case DUCKDB_TYPE_VARCHAR: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			ret = duckdb_create_varchar_length(duckdb_string_t_data(&s), duckdb_string_t_length(s));
			break;
		}
		case DUCKDB_TYPE_BLOB: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			ret = duckdb_create_blob((const uint8_t *)duckdb_string_t_data(&s), duckdb_string_t_length(s));
			break;
		}
		case DUCKDB_TYPE_BIT: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			duckdb_bit b;
			b.data = (uint8_t *)duckdb_string_t_data(&s);
			b.size = duckdb_string_t_length(s);
			ret = duckdb_create_bit(b);
			break;
		}

		case DUCKDB_TYPE_LIST: {
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			duckdb_vector child = duckdb_list_vector_get_child(vec);
			duckdb_logical_type ct = duckdb_list_type_child_type(lt);
			duckdb_value *vals = e.length ? emalloc(sizeof(duckdb_value) * e.length) : NULL;
			idx_t i;
			for (i = 0; i < e.length; i++) {
				vals[i] = pdo_duckdb_cell_to_value(child, e.offset + i);
			}
			ret = duckdb_create_list_value(ct, vals, e.length);
			for (i = 0; i < e.length; i++) {
				duckdb_destroy_value(&vals[i]);
			}
			if (vals) {
				efree(vals);
			}
			duckdb_destroy_logical_type(&ct);
			break;
		}
		case DUCKDB_TYPE_ARRAY: {
			idx_t n = duckdb_array_type_array_size(lt);
			duckdb_vector child = duckdb_array_vector_get_child(vec);
			duckdb_logical_type ct = duckdb_array_type_child_type(lt);
			duckdb_value *vals = n ? emalloc(sizeof(duckdb_value) * n) : NULL;
			idx_t base = row * n, i;
			for (i = 0; i < n; i++) {
				vals[i] = pdo_duckdb_cell_to_value(child, base + i);
			}
			ret = duckdb_create_array_value(ct, vals, n);
			for (i = 0; i < n; i++) {
				duckdb_destroy_value(&vals[i]);
			}
			if (vals) {
				efree(vals);
			}
			duckdb_destroy_logical_type(&ct);
			break;
		}
		case DUCKDB_TYPE_STRUCT: {
			idx_t n = duckdb_struct_type_child_count(lt);
			duckdb_value *vals = n ? emalloc(sizeof(duckdb_value) * n) : NULL;
			idx_t i;
			for (i = 0; i < n; i++) {
				duckdb_vector child = duckdb_struct_vector_get_child(vec, i);
				vals[i] = pdo_duckdb_cell_to_value(child, row);
			}
			ret = duckdb_create_struct_value(lt, vals);
			for (i = 0; i < n; i++) {
				duckdb_destroy_value(&vals[i]);
			}
			if (vals) {
				efree(vals);
			}
			break;
		}
		case DUCKDB_TYPE_MAP: {
			/* MAP is physically LIST<STRUCT<key, value>>. */
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			duckdb_vector entries = duckdb_list_vector_get_child(vec);
			duckdb_vector kvec = duckdb_struct_vector_get_child(entries, 0);
			duckdb_vector vvec = duckdb_struct_vector_get_child(entries, 1);
			duckdb_value *keys = e.length ? emalloc(sizeof(duckdb_value) * e.length) : NULL;
			duckdb_value *vals = e.length ? emalloc(sizeof(duckdb_value) * e.length) : NULL;
			idx_t i;
			for (i = 0; i < e.length; i++) {
				keys[i] = pdo_duckdb_cell_to_value(kvec, e.offset + i);
				vals[i] = pdo_duckdb_cell_to_value(vvec, e.offset + i);
			}
			ret = duckdb_create_map_value(lt, keys, vals, e.length);
			for (i = 0; i < e.length; i++) {
				duckdb_destroy_value(&keys[i]);
				duckdb_destroy_value(&vals[i]);
			}
			if (keys) {
				efree(keys);
			}
			if (vals) {
				efree(vals);
			}
			break;
		}

		default:
			/* Unknown/unsupported type: surface as SQL NULL rather than crash. */
			ret = duckdb_create_null_value();
			break;
	}

	duckdb_destroy_logical_type(&lt);
	return ret;
}
/* }}} */

static int pdo_duckdb_stmt_fetch(pdo_stmt_t *stmt,
	enum pdo_fetch_orientation ori, zend_long offset)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (!S->has_result || S->done) {
		return 0;
	}
	if (ori != PDO_FETCH_ORI_NEXT) {
		pdo_duckdb_error_stmt(stmt, "DuckDB PDO driver only supports forward-only cursors");
		return 0;
	}

	if (!S->started) {
		S->started = true;
	} else if (++S->cur < S->chunk_size) {
		return 1;
	}

	/* Advance to the next non-empty chunk. */
	for (;;) {
		if (S->chunk) {
			duckdb_destroy_data_chunk(&S->chunk);
			S->chunk = NULL;
		}
		S->chunk = duckdb_fetch_chunk(S->result);
		if (!S->chunk) {
			S->done = true;
			return 0;
		}
		S->chunk_size = duckdb_data_chunk_get_size(S->chunk);
		S->cur = 0;
		if (S->chunk_size > 0) {
			return 1;
		}
	}
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

/* Reconstruct the cell as a duckdb_value and render its canonical string into
 * `result`. Used for types without a native PHP mapping, and for integers that
 * overflow zend_long on 32-bit builds. */
static void pdo_duckdb_col_to_string(duckdb_vector vec, idx_t row, zval *result)
{
	duckdb_value v = pdo_duckdb_cell_to_value(vec, row);
	/* duckdb_get_varchar() on a NULL value throws a C++ InternalException that
	 * aborts the process; guard with is_null (also covers any unhandled type
	 * cell_to_value falls back to a NULL value for). */
	if (duckdb_is_null_value(v)) {
		ZVAL_NULL(result);
	} else {
		char *s = duckdb_get_varchar(v);
		if (s) {
			ZVAL_STRING(result, s);
			duckdb_free(s);
		} else {
			ZVAL_NULL(result);
		}
	}
	duckdb_destroy_value(&v);
}

static int pdo_duckdb_stmt_get_col(
		pdo_stmt_t *stmt, int colno, zval *result, enum pdo_param_type *type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	duckdb_vector vec;
	duckdb_type tid;
	uint64_t *validity;
	void *data;
	idx_t row;

	if (!S->chunk || S->cur >= S->chunk_size) {
		return 0;
	}
	if ((idx_t)colno >= duckdb_data_chunk_get_column_count(S->chunk)) {
		return 0;
	}

	row = S->cur;
	vec = duckdb_data_chunk_get_vector(S->chunk, (idx_t)colno);
	/* The result-level type id is returned by value (no allocation), unlike
	 * duckdb_vector_get_column_type() which allocates a logical type per call —
	 * this runs once per cell, so it must stay allocation-free on the hot path. */
	tid = duckdb_column_type(&S->result, (idx_t)colno);

	validity = duckdb_vector_get_validity(vec);
	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		ZVAL_NULL(result);
		return 1;
	}

	data = duckdb_vector_get_data(vec);

	switch (tid) {
		case DUCKDB_TYPE_BOOLEAN:   ZVAL_LONG(result, ((bool *)data)[row] ? 1 : 0); return 1;
		case DUCKDB_TYPE_TINYINT:   ZVAL_LONG(result, ((int8_t *)data)[row]); return 1;
		case DUCKDB_TYPE_SMALLINT:  ZVAL_LONG(result, ((int16_t *)data)[row]); return 1;
		case DUCKDB_TYPE_INTEGER:   ZVAL_LONG(result, ((int32_t *)data)[row]); return 1;
		case DUCKDB_TYPE_UTINYINT:  ZVAL_LONG(result, ((uint8_t *)data)[row]); return 1;
		case DUCKDB_TYPE_USMALLINT: ZVAL_LONG(result, ((uint16_t *)data)[row]); return 1;
		case DUCKDB_TYPE_FLOAT:     ZVAL_DOUBLE(result, (double)((float *)data)[row]); return 1;
		case DUCKDB_TYPE_DOUBLE:    ZVAL_DOUBLE(result, ((double *)data)[row]); return 1;

		case DUCKDB_TYPE_BIGINT: {
			int64_t v = ((int64_t *)data)[row];
#if SIZEOF_ZEND_LONG < 8
			if (v < (int64_t)ZEND_LONG_MIN || v > (int64_t)ZEND_LONG_MAX) {
				pdo_duckdb_col_to_string(vec, row, result);  /* preserve precision as string */
				return 1;
			}
#endif
			ZVAL_LONG(result, (zend_long)v);
			return 1;
		}
		case DUCKDB_TYPE_UINTEGER: {
			uint32_t v = ((uint32_t *)data)[row];
#if SIZEOF_ZEND_LONG < 8
			if (v > (uint32_t)ZEND_LONG_MAX) {
				pdo_duckdb_col_to_string(vec, row, result);
				return 1;
			}
#endif
			ZVAL_LONG(result, (zend_long)v);
			return 1;
		}

		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_BLOB: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			ZVAL_STRINGL(result, duckdb_string_t_data(&s), duckdb_string_t_length(s));
			return 1;
		}

		default:
			/* UBIGINT/HUGEINT/DECIMAL/temporal/UUID/ENUM/BIT and the nested
			 * types are returned as their canonical DuckDB string form. */
			pdo_duckdb_col_to_string(vec, row, result);
			return 1;
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

const struct pdo_stmt_methods duckdb_stmt_methods = {
	.dtor = pdo_duckdb_stmt_dtor,
	.executer = pdo_duckdb_stmt_execute,
	.fetcher = pdo_duckdb_stmt_fetch,
	.describer = pdo_duckdb_stmt_describe,
	.get_col = pdo_duckdb_stmt_get_col,
	.param_hook = pdo_duckdb_stmt_param_hook,
	.get_column_meta = pdo_duckdb_stmt_col_meta,
};
