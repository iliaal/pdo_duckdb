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

static void pdo_duckdb_appender_throw(duckdb_appender ap, const char *what)
{
	duckdb_error_data ed = ap ? duckdb_appender_error_data(ap) : NULL;
	const char *msg = (ed && duckdb_error_data_has_error(ed)) ? duckdb_error_data_message(ed) : NULL;

	zend_throw_exception_ex(php_pdo_get_exception(), 0, "%s: %s", what, msg ? msg : "unknown error");

	if (ed) {
		duckdb_destroy_error_data(&ed);
	}
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
	a->pdo = NULL;

	return &a->std;
}

static void pdo_duckdb_appender_free(zend_object *obj)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(obj);

	if (a->appender) {
		/* destroy flushes any outstanding rows; ignore errors at dtor time */
		if (!a->closed) {
			duckdb_appender_close(a->appender);
		}
		duckdb_appender_destroy(&a->appender);
		a->appender = NULL;
	}
	if (a->col_types) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			duckdb_destroy_logical_type(&a->col_types[c]);
		}
		efree(a->col_types);
		a->col_types = NULL;
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
	/* Modern PDO subclass model: a duckdb: DSN yields a Pdo\Duckdb instance, so
	 * its methods don't trip PHP 8.5's deprecation of base-PDO driver methods. */
	pdo_duckdb_ce = register_class_Pdo_Duckdb(php_pdo_get_dbh_ce());
	pdo_duckdb_ce->create_object = pdo_dbh_new;
	return php_pdo_register_driver_specific_ce(&pdo_duckdb_driver, pdo_duckdb_ce);
#else
	/* On 8.3 the method is exposed on the base PDO object via get_driver_methods
	 * (see below); the generated subclass registrar is unused there. */
	(void) register_class_Pdo_Duckdb;
	(void) pdo_duckdb_ce;
	return SUCCESS;
#endif
}

const zend_function_entry *pdo_duckdb_get_driver_methods(pdo_dbh_t *dbh, int kind)
{
	/* Always expose the method on base-PDO instances (`new PDO('duckdb:')`),
	 * for BC and for the 8.3 floor. On 8.4+ `PDO::connect()` additionally yields
	 * a Pdo\Duckdb instance whose own method does not trip the 8.5 deprecation
	 * of base-PDO driver methods. Same mechanism pdo_sqlite uses. */
	switch (kind) {
		case PDO_DBH_DRIVER_METHOD_KIND_DBH:
			return class_PdoDuckDb_Ext_methods;
		default:
			return NULL;
	}
}

/* {{{ duckdbAppender(string $table, ?string $schema = null): Pdo\Duckdb\Appender
 * Shared by the Pdo\Duckdb subclass method (8.4+) and the base-PDO
 * get_driver_methods vehicle (8.3). */
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

	/* duckdb_appender_create() takes NUL-terminated const char* identifiers, so
	 * an embedded NUL would silently truncate the table/schema name (e.g.
	 * "safe\0bad" would append to "safe"). Reject it. */
	if (zend_str_has_nul_byte(table) || (schema && zend_str_has_nul_byte(schema))) {
		zend_value_error("Pdo\\Duckdb\\Appender table and schema names must not contain a NUL byte");
		RETURN_THROWS();
	}

	/* Validate the optional column subset up front (before creating the appender):
	 * a non-empty list of NUL-free strings. Each name is added to the appender's
	 * active column list below; omitted columns then take their DEFAULT/NULL. */
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

	/* Restrict the appender to the requested columns, in order. add_column
	 * switches the appender from all-columns to the named active set; a bad
	 * column name fails here (DuckDB reports it). */
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

	/* Cache the target column logical types once, so appendRow() doesn't allocate
	 * one per cell on the bulk-load hot path. Used to route BLOB strings (binary
	 * vs UTF-8 varchar) and to build nested values from PHP arrays. */
	a->ncols = duckdb_appender_column_count(ap);
	if (a->ncols) {
		idx_t c;
		a->col_types = emalloc(sizeof(duckdb_logical_type) * a->ncols);
		for (c = 0; c < a->ncols; c++) {
			a->col_types[c] = duckdb_appender_column_type(ap, c);
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
	/* Not constructable from userland; created only via PDO::duckdbAppender(). */
	ZEND_PARSE_PARAMETERS_NONE();
	zend_throw_error(NULL, "Pdo\\Duckdb\\Appender cannot be constructed directly; use PDO::duckdbAppender()");
}

static pdo_duckdb_appender *pdo_duckdb_appender_live(zval *zthis)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(Z_OBJ_P(zthis));

	if (!a->appender || a->closed) {
		zend_throw_error(NULL, "Pdo\\Duckdb\\Appender is closed");
		return NULL;
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

/* Build a scalar leaf duckdb_value from a PHP scalar, targeting type `tid`.
 * Integer leaves are built at the column's exact width (range-checked, since
 * unlike the direct scalar append path DuckDB does not validate the cast for a
 * constructed value — an out-of-range narrow/unsigned value would silently wrap)
 * and float leaves match FLOAT vs DOUBLE; BLOB strings are built as a blob so
 * binary data round-trips; everything else (DATE/TIMESTAMP/DECIMAL/UUID via a
 * string) is built as its natural PHP form and cast by DuckDB when the row is
 * appended. Returns NULL on an unsupported zval type or an out-of-range integer
 * (throwing in the latter case); `argpos` is for the error message. */
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

/* Build a duckdb_value matching (or castable to) logical type `lt` from a PHP
 * value. A PHP array maps to LIST/ARRAY (sequential values) or STRUCT/MAP (per
 * the column type, keyed by field/key name); scalars map to leaves. Throws and
 * returns NULL on a structural mismatch (array for a scalar column, scalar for a
 * nested column, missing struct field, or wrong fixed-array length). `argpos` is
 * the 1-based appendRow() argument number, for error messages. */
static duckdb_value pdo_duckdb_build_value(zval *z, duckdb_logical_type lt, uint32_t argpos)
{
	duckdb_type tid = duckdb_get_type_id(lt);
	HashTable *ht;

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
			duckdb_logical_type ct = (tid == DUCKDB_TYPE_LIST)
				? duckdb_list_type_child_type(lt)
				: duckdb_array_type_child_type(lt);
			idx_t n = zend_hash_num_elements(ht);
			/* always non-NULL (even for an empty list) — create_list_value rejects
			 * a NULL values pointer. */
			duckdb_value *vals = emalloc(sizeof(duckdb_value) * (n ? n : 1));
			idx_t built = 0;
			duckdb_value ret = NULL;
			bool ok = true;
			zval *elem;

			if (tid == DUCKDB_TYPE_ARRAY) {
				idx_t want = duckdb_array_type_array_size(lt);
				if (n != want) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects %u fixed-array element(s), got %u",
						argpos, (uint32_t)want, (uint32_t)n);
					ok = false;
				}
			}

			if (ok) {
				ZEND_HASH_FOREACH_VAL(ht, elem) {
					duckdb_value cv = pdo_duckdb_build_value(elem, ct, argpos);
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
			duckdb_destroy_logical_type(&ct);
			return ret;
		}

		case DUCKDB_TYPE_STRUCT: {
			idx_t cnt = duckdb_struct_type_child_count(lt);
			duckdb_value *vals = cnt ? emalloc(sizeof(duckdb_value) * cnt) : NULL;
			idx_t i, built = 0;
			duckdb_value ret = NULL;
			bool ok = true;

			for (i = 0; i < cnt; i++) {
				char *fname = duckdb_struct_type_child_name(lt, i);
				duckdb_logical_type ft = duckdb_struct_type_child_type(lt, i);
				zval *fv = fname ? zend_hash_str_find(ht, fname, strlen(fname)) : NULL;
				duckdb_value cv;

				if (!fv) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is missing struct field \"%s\"",
						argpos, fname ? fname : "");
					if (fname) { duckdb_free(fname); }
					duckdb_destroy_logical_type(&ft);
					ok = false;
					break;
				}
				cv = pdo_duckdb_build_value(fv, ft, argpos);
				if (fname) { duckdb_free(fname); }
				duckdb_destroy_logical_type(&ft);
				if (!cv) { ok = false; break; }
				vals[built++] = cv;
			}

			if (ok) {
				ret = duckdb_create_struct_value(lt, vals);
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
			duckdb_logical_type kt = duckdb_map_type_key_type(lt);
			duckdb_logical_type vt = duckdb_map_type_value_type(lt);
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
				kv = pdo_duckdb_build_value(&ztmp, kt, argpos);
				if (!kv) { ok = false; break; }
				vv = pdo_duckdb_build_value(val, vt, argpos);
				if (!vv) { duckdb_destroy_value(&kv); ok = false; break; }
				keys[built] = kv;
				vals[built] = vv;
				built++;
			} ZEND_HASH_FOREACH_END();

			if (ok) {
				ret = duckdb_create_map_value(lt, keys, vals, n);
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
			duckdb_destroy_logical_type(&kt);
			duckdb_destroy_logical_type(&vt);
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

	/* Validate and pre-build the whole row before touching the native appender.
	 * DuckDB appends value-by-value with no rollback: a failure partway through
	 * (an unsupported PHP value, the wrong column count at end_row) leaves the
	 * appender mid-row and poisons it — later appends fail with "too many appends
	 * for chunk" and flush with "incomplete append to row". So check the column
	 * count, and construct every nested value (PHP array → LIST/STRUCT/MAP/ARRAY),
	 * up front; only once the whole row is known-good do we append. */
	if (argc != a->ncols) {
		zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): appender expects %u column(s), but %u value(s) were given",
			(uint32_t)a->ncols, argc);
		RETURN_THROWS();
	}

	/* built[i] holds a constructed duckdb_value for nested (array) cells, NULL for
	 * scalars handled directly on the append fast path below. */
	duckdb_value *built = argc ? ecalloc(argc, sizeof(duckdb_value)) : NULL;

	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		ZVAL_DEREF(v);
		switch (Z_TYPE_P(v)) {
			case IS_TRUE:
			case IS_FALSE:
			case IS_LONG:
			case IS_DOUBLE:
			case IS_STRING: {
				/* A non-NULL scalar for a nested column would otherwise reach the
				 * native append fast path and fail there — poisoning the appender
				 * if an earlier column in the row was already appended. Reject it
				 * up front, before anything is appended. (NULL is fine: it appends
				 * as a NULL of any column type.) */
				duckdb_type ctid = duckdb_get_type_id(a->col_types[i]);
				if (ctid == DUCKDB_TYPE_LIST || ctid == DUCKDB_TYPE_ARRAY ||
					ctid == DUCKDB_TYPE_STRUCT || ctid == DUCKDB_TYPE_MAP) {
					zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects an array for a nested column",
						i + 1);
					goto build_failed;
				}
				if (Z_TYPE_P(v) == IS_LONG &&
					!pdo_duckdb_validate_integer_range(Z_LVAL_P(v), ctid, i + 1)) {
					goto build_failed;
				}
				break;
			}
			case IS_NULL:
				break;
			case IS_ARRAY:
				/* nested column: build the value now; build_value throws on a
				 * structural mismatch. Nothing has been appended yet, so the
				 * appender is still pristine — just free what we built. */
				built[i] = pdo_duckdb_build_value(v, a->col_types[i], i + 1);
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

		if (built[i]) {
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
					/* IS_STRING. A PHP string maps to varchar by default, but
					 * DuckDB rejects non-UTF-8 there. If the target column is BLOB,
					 * append raw bytes so binary data round-trips. */
					bool is_blob = duckdb_get_type_id(a->col_types[i]) == DUCKDB_TYPE_BLOB;
					st = is_blob
						? duckdb_append_blob(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v))
						: duckdb_append_varchar_length(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v));
					break;
				}
			}
		}
		if (st != DuckDBSuccess) {
			/* A failed append invalidates the appender (DuckDB contract); mark it
			 * unusable so the free path destroys it instead of issuing more
			 * appends against a poisoned row. */
			a->closed = true;
			pdo_duckdb_appender_throw(a->appender, "Failed to append value");
			goto build_failed;
		}
	}

	for (i = 0; i < argc; i++) {
		if (built[i]) {
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
	/* Free any nested values constructed before the failure. An exception is
	 * already pending (build_value / the unsupported-type / append-failure path). */
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
		/* A failed flush invalidates the appender (DuckDB contract): no further
		 * appends are possible. Mark it unusable so the free path destroys it. */
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
		/* A failed close invalidates the appender (DuckDB contract); it's
		 * unusable either way, so mark it closed before surfacing the error. */
		a->closed = true;
		pdo_duckdb_appender_throw(a->appender, "Failed to close appender");
		RETURN_THROWS();
	}
	a->closed = true;
}
