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
	a->blob_cols = NULL;
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
	if (a->blob_cols) {
		efree(a->blob_cols);
		a->blob_cols = NULL;
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
	pdo_dbh_t *dbh;
	pdo_duckdb_db_handle *H;
	duckdb_appender ap = NULL;
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(table)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(schema)
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

	if (duckdb_appender_create(H->conn, schema ? ZSTR_VAL(schema) : NULL, ZSTR_VAL(table), &ap) != DuckDBSuccess) {
		pdo_duckdb_appender_throw(ap, "Unable to create DuckDB appender");
		if (ap) {
			duckdb_appender_destroy(&ap);
		}
		RETURN_THROWS();
	}

	object_init_ex(return_value, pdo_duckdb_appender_ce);
	a = pdo_duckdb_appender_from_obj(Z_OBJ_P(return_value));
	a->appender = ap;
	a->closed = false;
	a->pdo = Z_OBJ_P(ZEND_THIS);
	GC_ADDREF(a->pdo);

	/* Cache which target columns are BLOB once, so appendRow() doesn't allocate
	 * a logical type per string cell on the bulk-load hot path. */
	a->ncols = duckdb_appender_column_count(ap);
	if (a->ncols) {
		idx_t c;
		a->blob_cols = emalloc(sizeof(bool) * a->ncols);
		for (c = 0; c < a->ncols; c++) {
			duckdb_logical_type ct = duckdb_appender_column_type(ap, c);
			a->blob_cols[c] = duckdb_get_type_id(ct) == DUCKDB_TYPE_BLOB;
			duckdb_destroy_logical_type(&ct);
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

	/* Validate the whole row before touching the native appender. DuckDB appends
	 * value-by-value with no rollback: a failure partway through (a PHP value of
	 * an unsupported type, or the wrong column count caught at end_row) leaves
	 * the appender mid-row and poisons it — later appends fail with "too many
	 * appends for chunk" and flush with "incomplete append to row". So check the
	 * column count and every value's type up front, before any native append. */
	if (argc != a->ncols) {
		zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): table has %u column(s), but %u value(s) were given",
			(uint32_t)a->ncols, argc);
		RETURN_THROWS();
	}
	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		ZVAL_DEREF(v);
		switch (Z_TYPE_P(v)) {
			case IS_NULL:
			case IS_TRUE:
			case IS_FALSE:
			case IS_LONG:
			case IS_DOUBLE:
			case IS_STRING:
				break;
			default:
				zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is of unsupported type %s",
					i + 1, zend_zval_value_name(v));
				RETURN_THROWS();
		}
	}

	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		duckdb_state st;

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
				/* IS_STRING (the only remaining type after validation above). A
				 * PHP string maps to varchar by default, but DuckDB rejects
				 * non-UTF-8 there. If the target column is BLOB, append raw bytes
				 * so binary data round-trips. */
				bool is_blob = (i < a->ncols) && a->blob_cols[i];
				st = is_blob
					? duckdb_append_blob(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v))
					: duckdb_append_varchar_length(a->appender, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v));
				break;
			}
		}
		if (st != DuckDBSuccess) {
			/* A failed append invalidates the appender (DuckDB contract); mark it
			 * unusable so the free path destroys it instead of issuing more
			 * appends against a poisoned row. */
			a->closed = true;
			pdo_duckdb_appender_throw(a->appender, "Failed to append value");
			RETURN_THROWS();
		}
	}

	if (duckdb_appender_end_row(a->appender) != DuckDBSuccess) {
		a->closed = true;
		pdo_duckdb_appender_throw(a->appender, "Failed to end appender row");
		RETURN_THROWS();
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
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
