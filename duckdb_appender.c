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
#include "zend_exceptions.h"
#include "pdo_duckdb_arginfo.h"
#include "duckdb_driver_arginfo.h"

zend_class_entry *pdo_duckdb_appender_ce;
static zend_object_handlers pdo_duckdb_appender_handlers;
static zend_class_entry *pdo_duckdb_ce; /* the Pdo\Duckdb PDO subclass */

#define PDO_DUCKDB_APPENDER_COL_NESTED 1u
#define PDO_DUCKDB_APPENDER_COL_BLOB   2u

static void pdo_duckdb_appender_throw(duckdb_appender ap, const char *what)
{
	duckdb_error_data ed = ap ? duckdb_appender_error_data(ap) : NULL;
	const char *msg = (ed && duckdb_error_data_has_error(ed)) ? duckdb_error_data_message(ed) : NULL;

	zend_throw_exception_ex(php_pdo_get_exception(), 0, "%s: %s", what, msg ? msg : "unknown error");

	if (ed) {
		duckdb_destroy_error_data(&ed);
	}
}

static void pdo_duckdb_appender_warning(duckdb_appender ap, const char *what)
{
	duckdb_error_data ed = ap ? duckdb_appender_error_data(ap) : NULL;
	const char *msg = (ed && duckdb_error_data_has_error(ed)) ? duckdb_error_data_message(ed) : NULL;

	php_error_docref(NULL, E_WARNING, "%s: %s", what, msg ? msg : "unknown error");

	if (ed) {
		duckdb_destroy_error_data(&ed);
	}
}
/* Cache nested metadata once per appender. Scalar leaves borrow col_types[i];
 * child names use DuckDB's allocator. */
static void pdo_duckdb_appender_desc_destroy(pdo_duckdb_nested_render_type *t)
{
	idx_t i;

	if (!t) {
		return;
	}
	for (i = 0; i < t->child_count; i++) {
		pdo_duckdb_appender_desc_destroy(t->children[i]);
		if (t->child_names && t->child_names[i]) {
			duckdb_free(t->child_names[i]);
		}
	}
	if (t->children) {
		efree(t->children);
	}
	if (t->child_names) {
		efree(t->child_names);
	}
	if (t->owns_logical_type && t->logical_type) {
		duckdb_destroy_logical_type(&t->logical_type);
	}
	efree(t);
}

static pdo_duckdb_nested_render_type *pdo_duckdb_appender_desc_build(duckdb_type tid, duckdb_logical_type lt, bool owns_lt)
{
	pdo_duckdb_nested_render_type *t = ecalloc(1, sizeof(*t));
	idx_t i;

	t->type = tid;
	t->logical_type = lt;
	t->owns_logical_type = owns_lt;

	switch (tid) {
		case DUCKDB_TYPE_LIST: {
			duckdb_logical_type child = duckdb_list_type_child_type(lt);
			t->child_count = 1;
			t->children = ecalloc(1, sizeof(*t->children));
			t->children[0] = pdo_duckdb_appender_desc_build(duckdb_get_type_id(child), child, true);
			return t;
		}
		case DUCKDB_TYPE_ARRAY: {
			duckdb_logical_type child = duckdb_array_type_child_type(lt);
			t->child_count = 1;
			t->array_size = duckdb_array_type_array_size(lt);
			t->children = ecalloc(1, sizeof(*t->children));
			t->children[0] = pdo_duckdb_appender_desc_build(duckdb_get_type_id(child), child, true);
			return t;
		}
		case DUCKDB_TYPE_STRUCT:
			t->child_count = duckdb_struct_type_child_count(lt);
			t->children = ecalloc(t->child_count, sizeof(*t->children));
			t->child_names = ecalloc(t->child_count, sizeof(*t->child_names));
			for (i = 0; i < t->child_count; i++) {
				duckdb_logical_type child = duckdb_struct_type_child_type(lt, i);
				t->child_names[i] = duckdb_struct_type_child_name(lt, i);
				t->children[i] = pdo_duckdb_appender_desc_build(duckdb_get_type_id(child), child, true);
			}
			return t;
		case DUCKDB_TYPE_MAP: {
			duckdb_logical_type key = duckdb_map_type_key_type(lt);
			duckdb_logical_type value = duckdb_map_type_value_type(lt);
			t->child_count = 2;
			t->children = ecalloc(2, sizeof(*t->children));
			t->children[0] = pdo_duckdb_appender_desc_build(duckdb_get_type_id(key), key, true);
			t->children[1] = pdo_duckdb_appender_desc_build(duckdb_get_type_id(value), value, true);
			return t;
		}
		default:
			return t;
	}
}

/* CAST probes must match the exact target type, including DECIMAL width/scale.
 * Types without a probe retain native append-time validation. */
static bool pdo_duckdb_appender_probe_sql(duckdb_type tid, duckdb_logical_type lt, char **out_sql)
{
	const char *kw = NULL;

	*out_sql = NULL;
	switch (tid) {
		case DUCKDB_TYPE_BOOLEAN: kw = "BOOLEAN"; break;
		case DUCKDB_TYPE_TINYINT: kw = "TINYINT"; break;
		case DUCKDB_TYPE_SMALLINT: kw = "SMALLINT"; break;
		case DUCKDB_TYPE_INTEGER: kw = "INTEGER"; break;
		case DUCKDB_TYPE_BIGINT: kw = "BIGINT"; break;
		case DUCKDB_TYPE_UTINYINT: kw = "UTINYINT"; break;
		case DUCKDB_TYPE_USMALLINT: kw = "USMALLINT"; break;
		case DUCKDB_TYPE_UINTEGER: kw = "UINTEGER"; break;
		case DUCKDB_TYPE_UBIGINT: kw = "UBIGINT"; break;
		case DUCKDB_TYPE_HUGEINT: kw = "HUGEINT"; break;
		case DUCKDB_TYPE_UHUGEINT: kw = "UHUGEINT"; break;
		case DUCKDB_TYPE_FLOAT: kw = "FLOAT"; break;
		case DUCKDB_TYPE_DOUBLE: kw = "DOUBLE"; break;
		case DUCKDB_TYPE_DECIMAL:
			spprintf(out_sql, 0, "SELECT CAST(? AS DECIMAL(%u,%u))",
				(unsigned)duckdb_decimal_width(lt), (unsigned)duckdb_decimal_scale(lt));
			return true;
		case DUCKDB_TYPE_DATE: kw = "DATE"; break;
		case DUCKDB_TYPE_TIME: kw = "TIME"; break;
		case DUCKDB_TYPE_TIME_NS: kw = "TIME_NS"; break;
		case DUCKDB_TYPE_TIME_TZ: kw = "TIME_TZ"; break;
		case DUCKDB_TYPE_TIMESTAMP: kw = "TIMESTAMP"; break;
		case DUCKDB_TYPE_TIMESTAMP_S: kw = "TIMESTAMP_S"; break;
		case DUCKDB_TYPE_TIMESTAMP_MS: kw = "TIMESTAMP_MS"; break;
		case DUCKDB_TYPE_TIMESTAMP_NS: kw = "TIMESTAMP_NS"; break;
		case DUCKDB_TYPE_TIMESTAMP_TZ: kw = "TIMESTAMPTZ"; break;
		case DUCKDB_TYPE_INTERVAL: kw = "INTERVAL"; break;
		case DUCKDB_TYPE_UUID: kw = "UUID"; break;
		case DUCKDB_TYPE_BIT: kw = "BIT"; break;
		default: return false;
	}
	spprintf(out_sql, 0, "SELECT CAST(? AS %s)", kw);
	return true;
}

static duckdb_prepared_statement pdo_duckdb_appender_prepare_probe(duckdb_connection conn, duckdb_type tid, duckdb_logical_type lt)
{
	duckdb_prepared_statement probe = NULL;
	char *sql = NULL;

	if (!pdo_duckdb_appender_probe_sql(tid, lt, &sql)) {
		return NULL;
	}
	if (duckdb_prepare(conn, sql, &probe) != DuckDBSuccess) {
		/* Probe failure leaves validation to the native append. */
		if (probe) {
			duckdb_destroy_prepare(&probe);
		}
		efree(sql);
		return NULL;
	}
	efree(sql);
	return probe;
}

/* On failure stores an emalloc'd detail in *errp. */
static bool pdo_duckdb_appender_probe_value(duckdb_prepared_statement probe, const char *s, size_t len, char **errp)
{
	duckdb_result res;

	*errp = NULL;
	if (duckdb_bind_varchar_length(probe, 1, s, (idx_t)len) != DuckDBSuccess) {
		*errp = estrdup("unable to bind the cast probe");
		return false;
	}
	if (duckdb_execute_prepared(probe, &res) != DuckDBSuccess) {
		const char *e = duckdb_result_error(&res);
		*errp = estrdup(e ? e : "cast failed");
		duckdb_destroy_result(&res);
		return false;
	}
	duckdb_destroy_result(&res);
	return true;
}

static zend_object *pdo_duckdb_appender_new(zend_class_entry *ce)
{
	pdo_duckdb_appender *a = zend_object_alloc(sizeof(pdo_duckdb_appender), ce);

	zend_object_std_init(&a->std, ce);
	object_properties_init(&a->std, ce);
	a->std.handlers = &pdo_duckdb_appender_handlers;
	a->appender = NULL;
	a->closed = false;
	a->ncols = 0;
	a->col_types = NULL;
	a->col_type_ids = NULL;
	a->col_flags = NULL;
	a->col_desc = NULL;
	a->col_probes = NULL;
	a->pdo = NULL;

	return &a->std;
}

static void pdo_duckdb_appender_free(zend_object *obj)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(obj);

	if (a->appender) {
		duckdb_state close_state = DuckDBSuccess;
		bool was_closed = a->closed;
		/* destroy flushes outstanding rows; sandbox first so GC close cannot
		 * write outside a mid-request open_basedir tighten. */
		if (!a->closed) {
			if (a->pdo) {
				pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)
					php_pdo_dbh_fetch_inner(a->pdo)->driver_data;
				if (H) {
					(void)pdo_duckdb_enforce_sandbox(H);
				}
			}
			close_state = duckdb_appender_close(a->appender);
			if (close_state != DuckDBSuccess) {
				pdo_duckdb_appender_warning(a->appender,
					"Pdo\\Duckdb\\Appender close during destruction failed");
			}
		}
		if (duckdb_appender_destroy(&a->appender) != DuckDBSuccess && !was_closed && close_state == DuckDBSuccess) {
			php_error_docref(NULL, E_WARNING,
				"Pdo\\Duckdb\\Appender destroy during destruction failed");
		}
		a->appender = NULL;
	}
	/* Descriptors borrow col_types[i], so destroy them first. Probes need the
	 * connection to still be reachable. */
	if (a->col_desc) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			pdo_duckdb_appender_desc_destroy(a->col_desc[c]);
		}
		efree(a->col_desc);
		a->col_desc = NULL;
	}
	if (a->col_probes) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			if (a->col_probes[c]) {
				duckdb_destroy_prepare(&a->col_probes[c]);
			}
		}
		efree(a->col_probes);
		a->col_probes = NULL;
	}
	if (a->col_types) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			duckdb_destroy_logical_type(&a->col_types[c]);
		}
		efree(a->col_types);
		a->col_types = NULL;
	}
	if (a->col_type_ids) {
		efree(a->col_type_ids);
		a->col_type_ids = NULL;
	}
	if (a->col_flags) {
		efree(a->col_flags);
		a->col_flags = NULL;
	}
	if (a->pdo) {
		OBJ_RELEASE(a->pdo);
		a->pdo = NULL;
	}
	zend_object_std_dtor(obj);
}

/* The appender holds a reference to the PDO object (it owns the connection);
 * expose it so a cycle through that object stays collectable. */
static HashTable *pdo_duckdb_appender_get_gc(zend_object *obj, zval **table, int *n)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(obj);

	if (a->pdo) {
		zend_get_gc_buffer *buf = zend_get_gc_buffer_create();
		zend_get_gc_buffer_add_obj(buf, a->pdo);
		zend_get_gc_buffer_use(buf, table, n);
	} else {
		*table = NULL;
		*n = 0;
	}
	return zend_std_get_properties(obj);
}

zend_result pdo_duckdb_appender_minit(void)
{
	pdo_duckdb_appender_ce = register_class_Pdo_Duckdb_Appender();
	pdo_duckdb_appender_ce->create_object = pdo_duckdb_appender_new;

	memcpy(&pdo_duckdb_appender_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	pdo_duckdb_appender_handlers.offset = offsetof(pdo_duckdb_appender, std);
	pdo_duckdb_appender_handlers.free_obj = pdo_duckdb_appender_free;
	pdo_duckdb_appender_handlers.clone_obj = NULL;
	pdo_duckdb_appender_handlers.get_gc = pdo_duckdb_appender_get_gc;

#if PHP_VERSION_ID >= 80400
	pdo_duckdb_ce = register_class_Pdo_Duckdb(php_pdo_get_dbh_ce());
	pdo_duckdb_ce->create_object = pdo_dbh_new;
	return php_pdo_register_driver_specific_ce(&pdo_duckdb_driver, pdo_duckdb_ce);
#else
	/* 8.1-8.3 use get_driver_methods instead of the generated subclass. */
	(void) register_class_Pdo_Duckdb;
	(void) pdo_duckdb_ce;
	return SUCCESS;
#endif
}

const zend_function_entry *pdo_duckdb_get_driver_methods(pdo_dbh_t *dbh, int kind)
{
	/* Base-PDO instances (`new PDO('duckdb:')`) always get the methods, for BC
	 * and for 8.1-8.3, as in pdo_sqlite. */
	switch (kind) {
		case PDO_DBH_DRIVER_METHOD_KIND_DBH:
			return class_PdoDuckDb_Ext_methods;
		default:
			return NULL;
	}
}

/* {{{ duckdbAppender(string $table, ?string $schema = null, ?array $columns = null): Pdo\Duckdb\Appender */
static void pdo_duckdb_appender_create_impl(INTERNAL_FUNCTION_PARAMETERS)
{
	zend_string *table;
	zend_string *schema = NULL;
	HashTable *columns = NULL;
	pdo_dbh_t *dbh;
	pdo_duckdb_db_handle *H;
	duckdb_appender ap = NULL;
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_STR(table)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(schema)
		Z_PARAM_ARRAY_HT_OR_NULL(columns)
	ZEND_PARSE_PARAMETERS_END();

	dbh = Z_PDO_DBH_P(ZEND_THIS);
	PDO_CONSTRUCT_CHECK;
	H = (pdo_duckdb_db_handle *)dbh->driver_data;

	if (!pdo_duckdb_enforce_sandbox(H)) {
		zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
			"PDO::duckdbAppender(): unable to apply the open_basedir sandbox");
		RETURN_THROWS();
	}

	/* duckdb_appender_create() would truncate "safe\0bad" to "safe". */
	if (zend_str_has_nul_byte(table) || (schema && zend_str_has_nul_byte(schema))) {
		zend_value_error("Pdo\\Duckdb\\Appender table and schema names must not contain a NUL byte");
		RETURN_THROWS();
	}

	if (columns) {
		zval *col;

		if (zend_hash_num_elements(columns) == 0) {
			zend_value_error("Pdo\\Duckdb\\Appender column list must not be empty");
			RETURN_THROWS();
		}
		ZEND_HASH_FOREACH_VAL(columns, col) {
			ZVAL_DEREF(col);
			if (Z_TYPE_P(col) != IS_STRING) {
				zend_type_error("Pdo\\Duckdb\\Appender column names must be strings");
				RETURN_THROWS();
			}
			if (zend_str_has_nul_byte(Z_STR_P(col))) {
				zend_value_error("Pdo\\Duckdb\\Appender column names must not contain a NUL byte");
				RETURN_THROWS();
			}
		} ZEND_HASH_FOREACH_END();
	}

	if (duckdb_appender_create(H->conn, schema ? ZSTR_VAL(schema) : NULL, ZSTR_VAL(table), &ap) != DuckDBSuccess) {
		pdo_duckdb_appender_throw(ap, "Unable to create DuckDB appender");
		if (ap) {
			duckdb_appender_destroy(&ap);
		}
		RETURN_THROWS();
	}

	if (columns) {
		zval *col;

		ZEND_HASH_FOREACH_VAL(columns, col) {
			ZVAL_DEREF(col);
			if (duckdb_appender_add_column(ap, Z_STRVAL_P(col)) != DuckDBSuccess) {
				pdo_duckdb_appender_throw(ap, "Unable to add appender column");
				duckdb_appender_destroy(&ap);
				RETURN_THROWS();
			}
		} ZEND_HASH_FOREACH_END();
	}

	object_init_ex(return_value, pdo_duckdb_appender_ce);
	a = pdo_duckdb_appender_from_obj(Z_OBJ_P(return_value));
	a->appender = ap;
	a->closed = false;
	a->pdo = Z_OBJ_P(ZEND_THIS);
	GC_ADDREF(a->pdo);

	/* Cached so appendRow() doesn't allocate a logical type per cell. */
	a->ncols = duckdb_appender_column_count(ap);
	if (a->ncols) {
		idx_t c;
		a->col_types = emalloc(sizeof(duckdb_logical_type) * a->ncols);
		a->col_type_ids = emalloc(sizeof(duckdb_type) * a->ncols);
		a->col_flags = ecalloc(a->ncols, sizeof(unsigned char));
		for (c = 0; c < a->ncols; c++) {
			duckdb_type tid;
			a->col_types[c] = duckdb_appender_column_type(ap, c);
			tid = duckdb_get_type_id(a->col_types[c]);
			a->col_type_ids[c] = tid;
			if (tid == DUCKDB_TYPE_LIST || tid == DUCKDB_TYPE_ARRAY ||
				tid == DUCKDB_TYPE_STRUCT || tid == DUCKDB_TYPE_MAP) {
				a->col_flags[c] |= PDO_DUCKDB_APPENDER_COL_NESTED;
			}
			if (tid == DUCKDB_TYPE_BLOB) {
				a->col_flags[c] |= PDO_DUCKDB_APPENDER_COL_BLOB;
			}
		}
		/* One cached "SELECT CAST(? AS <T>)" probe per spellable scalar
		 * column; a type the probe can't spell fails at append instead. */
		a->col_desc = ecalloc(a->ncols, sizeof(pdo_duckdb_nested_render_type *));
		a->col_probes = ecalloc(a->ncols, sizeof(duckdb_prepared_statement));
		for (c = 0; c < a->ncols; c++) {
			a->col_desc[c] = pdo_duckdb_appender_desc_build(
				a->col_type_ids[c], a->col_types[c], false);
			if (!(a->col_flags[c] & PDO_DUCKDB_APPENDER_COL_NESTED)) {
				a->col_probes[c] = pdo_duckdb_appender_prepare_probe(
					H->conn, a->col_type_ids[c], a->col_types[c]);
			}
		}
	}
}

ZEND_METHOD(Pdo_Duckdb, duckdbAppender)
{
	pdo_duckdb_appender_create_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

ZEND_METHOD(PdoDuckDb_Ext, duckdbAppender)
{
	pdo_duckdb_appender_create_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

ZEND_METHOD(Pdo_Duckdb_Appender, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();
	zend_throw_error(NULL, "Pdo\\Duckdb\\Appender cannot be constructed directly; use PDO::duckdbAppender()");
}

/* Throws Error when closed; re-applies the sandbox if open_basedir tightened
 * after create. */
static pdo_duckdb_appender *pdo_duckdb_appender_live(zval *zthis)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(Z_OBJ_P(zthis));
	pdo_duckdb_db_handle *H;

	if (!a->appender || a->closed) {
		zend_throw_error(NULL, "Pdo\\Duckdb\\Appender is closed");
		return NULL;
	}
	if (a->pdo) {
		H = (pdo_duckdb_db_handle *)php_pdo_dbh_fetch_inner(a->pdo)->driver_data;
		if (H && !pdo_duckdb_enforce_sandbox(H)) {
			zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
				"Pdo\\Duckdb\\Appender: unable to apply the open_basedir sandbox");
			return NULL;
		}
	}
	return a;
}

static bool pdo_duckdb_validate_integer_range(zend_long l, duckdb_type tid, uint32_t argpos)
{
	switch (tid) {
		case DUCKDB_TYPE_TINYINT:
			if (l >= INT8_MIN && l <= INT8_MAX) { return true; }
			break;
		case DUCKDB_TYPE_SMALLINT:
			if (l >= INT16_MIN && l <= INT16_MAX) { return true; }
			break;
		case DUCKDB_TYPE_INTEGER:
			if (l >= INT32_MIN && l <= INT32_MAX) { return true; }
			break;
		case DUCKDB_TYPE_UTINYINT:
			if (l >= 0 && l <= UINT8_MAX) { return true; }
			break;
		case DUCKDB_TYPE_USMALLINT:
			if (l >= 0 && l <= UINT16_MAX) { return true; }
			break;
		case DUCKDB_TYPE_UINTEGER:
			if (l >= 0 && (uint64_t)l <= UINT32_MAX) { return true; }
			break;
		case DUCKDB_TYPE_UBIGINT:
		case DUCKDB_TYPE_UHUGEINT:
			if (l >= 0) { return true; }
			break;
		default:
			return true;
	}

	zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u integer "
		ZEND_LONG_FMT " is out of range for the target column type", argpos, l);
	return false;
}

/* Constructed integer values bypass DuckDB's cast validation: range-check
 * before narrowing to avoid silent wraparound. */
static duckdb_value pdo_duckdb_make_leaf(zval *z, duckdb_type tid, uint32_t argpos)
{
	switch (Z_TYPE_P(z)) {
		case IS_NULL:  return duckdb_create_null_value();
		case IS_TRUE:  return duckdb_create_bool(true);
		case IS_FALSE: return duckdb_create_bool(false);
		case IS_DOUBLE:
			return tid == DUCKDB_TYPE_FLOAT
				? duckdb_create_float((float)Z_DVAL_P(z))
				: duckdb_create_double(Z_DVAL_P(z));
		case IS_STRING:
			return tid == DUCKDB_TYPE_BLOB
				? duckdb_create_blob((const uint8_t *)Z_STRVAL_P(z), (idx_t)Z_STRLEN_P(z))
				: duckdb_create_varchar_length(Z_STRVAL_P(z), (idx_t)Z_STRLEN_P(z));
		case IS_LONG: {
			zend_long l = Z_LVAL_P(z);
			if (!pdo_duckdb_validate_integer_range(l, tid, argpos)) {
				return NULL;
			}
			switch (tid) {
				case DUCKDB_TYPE_BOOLEAN:   return duckdb_create_bool(l != 0);
				case DUCKDB_TYPE_TINYINT:   return duckdb_create_int8((int8_t)l);
				case DUCKDB_TYPE_SMALLINT:  return duckdb_create_int16((int16_t)l);
				case DUCKDB_TYPE_INTEGER:   return duckdb_create_int32((int32_t)l);
				case DUCKDB_TYPE_UTINYINT:  return duckdb_create_uint8((uint8_t)l);
				case DUCKDB_TYPE_USMALLINT: return duckdb_create_uint16((uint16_t)l);
				case DUCKDB_TYPE_UINTEGER:  return duckdb_create_uint32((uint32_t)l);
				case DUCKDB_TYPE_UBIGINT:   return duckdb_create_uint64((uint64_t)l);
				case DUCKDB_TYPE_FLOAT:     return duckdb_create_float((float)l);
				case DUCKDB_TYPE_DOUBLE:    return duckdb_create_double((double)l);
				case DUCKDB_TYPE_HUGEINT: {
					duckdb_hugeint h;
					h.lower = (uint64_t)l;
					h.upper = l < 0 ? -1 : 0;
					return duckdb_create_hugeint(h);
				}
				case DUCKDB_TYPE_UHUGEINT: {
					duckdb_uhugeint h;
					h.lower = (uint64_t)l;
					h.upper = 0;
					return duckdb_create_uhugeint(h);
				}
				default:                    return duckdb_create_int64((int64_t)l);
			}
		}
		default:
			return NULL;
	}
}

#define PDO_DUCKDB_APPENDER_MAX_DEPTH 128

/* depth starts at zero; argpos is the 1-based appendRow() argument number. */
static duckdb_value pdo_duckdb_build_value(zval *z, const pdo_duckdb_nested_render_type *t, uint32_t argpos, int depth)
{
	duckdb_type tid = t->type;
	HashTable *ht;

	if (depth > PDO_DUCKDB_APPENDER_MAX_DEPTH) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"Pdo\\Duckdb\\Appender::appendRow(): maximum nesting depth (128) exceeded");
		return NULL;
	}

	ZVAL_DEREF(z);

	if (Z_TYPE_P(z) == IS_NULL) {
		return duckdb_create_null_value();
	}

	if (Z_TYPE_P(z) != IS_ARRAY) {
		duckdb_value v;
		if (tid == DUCKDB_TYPE_LIST || tid == DUCKDB_TYPE_ARRAY ||
			tid == DUCKDB_TYPE_STRUCT || tid == DUCKDB_TYPE_MAP) {
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects an array for a nested column", argpos);
			return NULL;
		}
		v = pdo_duckdb_make_leaf(z, tid, argpos);
		if (!v && !EG(exception)) {
			/* make_leaf throws on an out-of-range integer; only report an
			 * unsupported type when it returned NULL without throwing. */
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is of unsupported type %s",
				argpos, zend_zval_value_name(z));
		}
		return v;
	}

	ht = Z_ARRVAL_P(z);

	switch (tid) {
		case DUCKDB_TYPE_LIST:
		case DUCKDB_TYPE_ARRAY: {
			/* Borrowed from the cached descriptor; never destroyed here. */
			duckdb_logical_type ct = t->children[0]->logical_type;
			idx_t n = zend_hash_num_elements(ht);

			if (!zend_array_is_list(ht)) {
				zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects a list-shaped array",
					argpos);
				return NULL;
			}
			/* create_list_value rejects a NULL values pointer, even for an empty
			 * list. */
			duckdb_value *vals = emalloc(sizeof(duckdb_value) * (n ? n : 1));
			idx_t built = 0;
			duckdb_value ret = NULL;
			bool ok = true;
			zval *elem;

			if (tid == DUCKDB_TYPE_ARRAY) {
				idx_t want = t->array_size;
				if (n != want) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects %u fixed-array element(s), got %u",
						argpos, (uint32_t)want, (uint32_t)n);
					ok = false;
				}
			}

			if (ok) {
				ZEND_HASH_FOREACH_VAL(ht, elem) {
					duckdb_value cv = pdo_duckdb_build_value(elem, t->children[0], argpos, depth + 1);
					if (!cv) { ok = false; break; }
					vals[built++] = cv;
				} ZEND_HASH_FOREACH_END();
			}

			if (ok) {
				ret = (tid == DUCKDB_TYPE_LIST)
					? duckdb_create_list_value(ct, vals, n)
					: duckdb_create_array_value(ct, vals, n);
				if (!ret) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u could not be built as a nested value", argpos);
				}
			}
			while (built > 0) {
				duckdb_destroy_value(&vals[--built]);
			}
			efree(vals);
			return ret;
		}

		case DUCKDB_TYPE_STRUCT: {
			idx_t cnt = t->child_count;
			duckdb_value *vals;
			idx_t i, built = 0;
			duckdb_value ret = NULL;
			bool ok = true;

			if (zend_hash_num_elements(ht) != cnt) {
				zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects exactly %u struct field(s), got %u",
					argpos, (uint32_t)cnt, (uint32_t)zend_hash_num_elements(ht));
				return NULL;
			}
			vals = cnt ? emalloc(sizeof(duckdb_value) * cnt) : NULL;

			for (i = 0; i < cnt; i++) {
				const char *fname = (t->child_names && t->child_names[i]) ? t->child_names[i] : NULL;
				zval *fv = fname ? zend_symtable_str_find(ht, fname, strlen(fname)) : NULL;
				duckdb_value cv;

				if (!fv) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is missing struct field \"%s\"",
						argpos, fname ? fname : "");
					ok = false;
					break;
				}
				cv = pdo_duckdb_build_value(fv, t->children[i], argpos, depth + 1);
				if (!cv) { ok = false; break; }
				vals[built++] = cv;
			}

			if (ok) {
				ret = duckdb_create_struct_value(t->logical_type, vals);
				if (!ret) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u could not be built as a struct", argpos);
				}
			}
			while (built > 0) {
				duckdb_destroy_value(&vals[--built]);
			}
			if (vals) {
				efree(vals);
			}
			return ret;
		}

		case DUCKDB_TYPE_MAP: {
			idx_t n = zend_hash_num_elements(ht);
			duckdb_value *keys = emalloc(sizeof(duckdb_value) * (n ? n : 1));
			duckdb_value *vals = emalloc(sizeof(duckdb_value) * (n ? n : 1));
			idx_t built = 0;
			duckdb_value ret = NULL;
			bool ok = true;
			zend_string *skey;
			zend_ulong nkey;
			zval *val;

			ZEND_HASH_FOREACH_KEY_VAL(ht, nkey, skey, val) {
				duckdb_value kv, vv;
				zval ztmp;

				if (skey) {
					ZVAL_STR(&ztmp, skey);
				} else {
					ZVAL_LONG(&ztmp, (zend_long)nkey);
				}
				kv = pdo_duckdb_build_value(&ztmp, t->children[0], argpos, depth + 1);
				if (!kv) { ok = false; break; }
				vv = pdo_duckdb_build_value(val, t->children[1], argpos, depth + 1);
				if (!vv) { duckdb_destroy_value(&kv); ok = false; break; }
				keys[built] = kv;
				vals[built] = vv;
				built++;
			} ZEND_HASH_FOREACH_END();

			if (ok) {
				ret = duckdb_create_map_value(t->logical_type, keys, vals, n);
				if (!ret) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u could not be built as a map", argpos);
				}
			}
			while (built > 0) {
				--built;
				duckdb_destroy_value(&keys[built]);
				duckdb_destroy_value(&vals[built]);
			}
			efree(keys);
			efree(vals);
			return ret;
		}

		default:
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is an array but the target column is not a nested type", argpos);
			return NULL;
	}
}

ZEND_METHOD(Pdo_Duckdb_Appender, appendRow)
{
	zval *args = NULL;
	uint32_t argc = 0, i;
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_START(0, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	a = pdo_duckdb_appender_live(ZEND_THIS);
	if (!a) {
		RETURN_THROWS();
	}

	/* Validate the whole row first: DuckDB cannot roll back a partial append,
	 * and a mid-row failure makes the appender unusable. */
	if (argc != a->ncols) {
		zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): appender expects %u column(s), but %u value(s) were given",
			(uint32_t)a->ncols, argc);
		RETURN_THROWS();
	}

	/* built[i] holds a constructed duckdb_value for nested (array) cells, NULL for
	 * scalars handled directly on the append fast path below. */
	duckdb_value *built = NULL;

	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		ZVAL_DEREF(v);
		switch (Z_TYPE_P(v)) {
			case IS_TRUE:
			case IS_FALSE:
			case IS_LONG:
			case IS_DOUBLE:
			case IS_STRING: {
				duckdb_type ctid = a->col_type_ids[i];
				if (a->col_flags[i] & PDO_DUCKDB_APPENDER_COL_NESTED) {
					zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects an array for a nested column",
						i + 1);
					goto build_failed;
				}
				if (Z_TYPE_P(v) == IS_LONG &&
					!pdo_duckdb_validate_integer_range(Z_LVAL_P(v), ctid, i + 1)) {
					goto build_failed;
				}
				if (Z_TYPE_P(v) == IS_STRING && a->col_probes[i]) {
					char *perr = NULL;
					if (!pdo_duckdb_appender_probe_value(a->col_probes[i],
							Z_STRVAL_P(v), Z_STRLEN_P(v), &perr)) {
						zend_throw_exception_ex(php_pdo_get_exception(), 0,
							"Failed to append value: %s", perr ? perr : "unknown error");
						if (perr) {
							efree(perr);
						}
						goto build_failed;
					}
				}
				break;
			}
			case IS_NULL:
				break;
			case IS_ARRAY:
				if (!built) {
					built = ecalloc(argc, sizeof(duckdb_value));
				}
				built[i] = pdo_duckdb_build_value(v, a->col_desc[i], i + 1, 0);
				if (!built[i]) {
					goto build_failed;
				}
				break;
			default:
				zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is of unsupported type %s",
					i + 1, zend_zval_value_name(v));
				goto build_failed;
		}
	}

	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		duckdb_state st;

		if (built && built[i]) {
			st = duckdb_append_value(a->appender, built[i]);
		} else {
			ZVAL_DEREF(v);
			switch (Z_TYPE_P(v)) {
				case IS_NULL:
					st = duckdb_append_null(a->appender);
					break;
				case IS_TRUE:
				case IS_FALSE:
					st = duckdb_append_bool(a->appender, Z_TYPE_P(v) == IS_TRUE);
					break;
				case IS_LONG:
					st = duckdb_append_int64(a->appender, (int64_t)Z_LVAL_P(v));
					break;
				case IS_DOUBLE:
					st = duckdb_append_double(a->appender, Z_DVAL_P(v));
					break;
				default: {
					/* DuckDB rejects non-UTF-8 VARCHAR, so BLOB columns get raw
					 * bytes. */
					st = (a->col_flags[i] & PDO_DUCKDB_APPENDER_COL_BLOB)
						? duckdb_append_blob(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v))
						: duckdb_append_varchar_length(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v));
					break;
				}
			}
		}
		if (st != DuckDBSuccess) {
			/* DuckDB invalidates the appender after a failed append. */
			a->closed = true;
			pdo_duckdb_appender_throw(a->appender, "Failed to append value");
			goto build_failed;
		}
	}

	for (i = 0; i < argc; i++) {
		if (built && built[i]) {
			duckdb_destroy_value(&built[i]);
		}
	}
	if (built) {
		efree(built);
	}

	if (duckdb_appender_end_row(a->appender) != DuckDBSuccess) {
		a->closed = true;
		pdo_duckdb_appender_throw(a->appender, "Failed to end appender row");
		RETURN_THROWS();
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));

build_failed:
	if (built) {
		for (i = 0; i < argc; i++) {
			if (built[i]) {
				duckdb_destroy_value(&built[i]);
			}
		}
		efree(built);
	}
	RETURN_THROWS();
}

ZEND_METHOD(Pdo_Duckdb_Appender, flush)
{
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_NONE();

	a = pdo_duckdb_appender_live(ZEND_THIS);
	if (!a) {
		RETURN_THROWS();
	}
	if (duckdb_appender_flush(a->appender) != DuckDBSuccess) {
		/* DuckDB invalidates the appender after a failed flush. */
		a->closed = true;
		pdo_duckdb_appender_throw(a->appender, "Failed to flush appender");
		RETURN_THROWS();
	}
}

ZEND_METHOD(Pdo_Duckdb_Appender, close)
{
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_NONE();

	a = pdo_duckdb_appender_live(ZEND_THIS);
	if (!a) {
		RETURN_THROWS();
	}
	if (duckdb_appender_close(a->appender) != DuckDBSuccess) {
		a->closed = true;
		pdo_duckdb_appender_throw(a->appender, "Failed to close appender");
		RETURN_THROWS();
	}
	a->closed = true;
}
