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
#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_variant.h"
#include "pdo_duckdb_bridge_appender.h"

zend_class_entry *pdo_duckdb_appender_ce;
static zend_object_handlers pdo_duckdb_appender_handlers;
static zend_class_entry *pdo_duckdb_ce; /* the Pdo\Duckdb PDO subclass */

#define PDO_DUCKDB_APPENDER_COL_NESTED 1u
#define PDO_DUCKDB_APPENDER_COL_BLOB   2u

/* Per-column, per-scalar-kind pre-append validation, indexed by
 * Z_TYPE - IS_FALSE (false, true, long, double, string). */
#define PDO_DUCKDB_CHK_NONE       0u
#define PDO_DUCKDB_CHK_PROBE      1u /* exact CAST probe */
#define PDO_DUCKDB_CHK_BOUNDS     2u /* probe only outside col_num_bounds */
#define PDO_DUCKDB_CHK_INT_RANGE  3u /* pdo_duckdb_validate_integer_range() */
#define PDO_DUCKDB_CHK_FLOAT      4u /* DOUBLE -> FLOAT overflow */
#define PDO_DUCKDB_CHK_NOT_STRING 5u /* BLOB takes only strings */
#define PDO_DUCKDB_CHK_NESTED     6u /* nested columns take only arrays */
#define PDO_DUCKDB_CHK_KINDS      5

typedef struct {
	pdo_duckdb_nested_render_type type;
	bool tuple;
	pdd_bridge_bytes *names;
} pdo_duckdb_appender_desc;

static void pdo_duckdb_appender_bridge_error(pdd_bridge_error *error)
{
	zend_try {
		zend_throw_exception_ex(php_pdo_get_exception(), 0, "Pdo\\Duckdb\\Appender: %s",
			error->data ? error->data : "unable to construct DuckDB value");
	} zend_catch {
		pdd_bridge_error_free(error);
		zend_bailout();
	} zend_end_try();
	pdd_bridge_error_free(error);
}

static void pdo_duckdb_appender_report(duckdb_error_data ed, const char *what, bool warning)
{
	const char *msg = (ed && duckdb_error_data_has_error(ed)) ? duckdb_error_data_message(ed) : NULL;

	zend_try {
		if (warning) {
			php_error_docref(NULL, E_WARNING, "%s: %s", what, msg ? msg : "unknown error");
		} else {
			zend_throw_exception_ex(php_pdo_get_exception(), 0, "%s: %s", what, msg ? msg : "unknown error");
		}
	} zend_catch {
		if (ed) {
			duckdb_destroy_error_data(&ed);
		}
		zend_bailout();
	} zend_end_try();
	if (ed) {
		duckdb_destroy_error_data(&ed);
	}
}

static void pdo_duckdb_appender_throw(pdo_duckdb_db_handle *H, duckdb_appender ap, const char *what)
{
	duckdb_error_data ed;

	pdo_duckdb_instance_lock(H);
	ed = ap ? duckdb_appender_error_data(ap) : NULL;
	pdo_duckdb_instance_unlock(H);
	pdo_duckdb_appender_report(ed, what, false);
}

static bool pdo_duckdb_appender_enter(pdo_duckdb_db_handle *H)
{
	if (pdo_duckdb_native_enter(H)) {
		return true;
	}
	zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
		"Pdo\\Duckdb\\Appender: %s", pdo_duckdb_sandbox_error(H));
	return false;
}
/* Cache nested metadata once per appender. Scalar leaves borrow col_types[i];
 * field names own bridge byte buffers. */
static void pdo_duckdb_appender_desc_destroy(pdo_duckdb_nested_render_type *t)
{
	idx_t i;
	pdo_duckdb_appender_desc *desc = (pdo_duckdb_appender_desc *)t;

	if (!t) {
		return;
	}
	for (i = 0; i < t->child_count; i++) {
		pdo_duckdb_appender_desc_destroy(t->children[i]);
		if (desc->names) {
			pdd_bridge_bytes_free(&desc->names[i]);
		}
	}
	if (t->children) {
		efree(t->children);
	}
	if (desc->names) {
		efree(desc->names);
	}
	if (t->owns_logical_type && t->logical_type) {
		duckdb_destroy_logical_type(&t->logical_type);
	}
	efree(t);
}

/* Each node is stored in its parent slot before anything inside it is
 * allocated, and child_count is set only once the child arrays exist, so a
 * memory_limit bailout leaves a tree pdo_duckdb_appender_desc_destroy() can
 * free, including the DuckDB-allocated child types and field names. */
static pdo_duckdb_nested_render_type *pdo_duckdb_appender_desc_node(pdo_duckdb_nested_render_type **slot)
{
	*slot = ecalloc(1, sizeof(pdo_duckdb_appender_desc));
	return *slot;
}

static void pdo_duckdb_appender_desc_child(pdo_duckdb_nested_render_type **slot, duckdb_logical_type parent,
	duckdb_type parent_tid, idx_t i);

static void pdo_duckdb_appender_desc_fill(pdo_duckdb_nested_render_type *t)
{
	idx_t i, n;
	pdo_duckdb_appender_desc *desc = (pdo_duckdb_appender_desc *)t;
	pdd_bridge_error error = {0};

	switch (t->type) {
		case DUCKDB_TYPE_ARRAY:
			t->array_size = duckdb_array_type_array_size(t->logical_type);
			ZEND_FALLTHROUGH;
		case DUCKDB_TYPE_LIST:
			t->children = ecalloc(1, sizeof(*t->children));
			t->child_count = 1;
			pdo_duckdb_appender_desc_child(&t->children[0], t->logical_type, t->type, 0);
			return;
		case DUCKDB_TYPE_STRUCT:
			if (pdd_bridge_type_is_tuple(t->logical_type, &desc->tuple, &error) != PDD_BRIDGE_OK) {
				pdo_duckdb_appender_bridge_error(&error);
				return;
			}
			n = duckdb_struct_type_child_count(t->logical_type);
			t->children = ecalloc(n, sizeof(*t->children));
			desc->names = ecalloc(n, sizeof(*desc->names));
			t->child_count = n;
			for (i = 0; i < n; i++) {
				if (pdd_bridge_type_field_name(t->logical_type, i, &desc->names[i], &error) != PDD_BRIDGE_OK) {
					pdo_duckdb_appender_bridge_error(&error);
					return;
				}
				pdo_duckdb_appender_desc_child(&t->children[i], t->logical_type, t->type, i);
				if (EG(exception)) {
					return;
				}
			}
			return;
		case DUCKDB_TYPE_MAP:
			t->children = ecalloc(2, sizeof(*t->children));
			t->child_count = 2;
			pdo_duckdb_appender_desc_child(&t->children[0], t->logical_type, t->type, 0);
			if (EG(exception)) {
				return;
			}
			pdo_duckdb_appender_desc_child(&t->children[1], t->logical_type, t->type, 1);
			return;
		default:
			return;
	}
}

static void pdo_duckdb_appender_desc_child(pdo_duckdb_nested_render_type **slot, duckdb_logical_type parent,
	duckdb_type parent_tid, idx_t i)
{
	pdo_duckdb_nested_render_type *t = pdo_duckdb_appender_desc_node(slot);

	switch (parent_tid) {
		case DUCKDB_TYPE_LIST:
			t->logical_type = duckdb_list_type_child_type(parent);
			break;
		case DUCKDB_TYPE_ARRAY:
			t->logical_type = duckdb_array_type_child_type(parent);
			break;
		case DUCKDB_TYPE_MAP:
			t->logical_type = i == 0 ? duckdb_map_type_key_type(parent) : duckdb_map_type_value_type(parent);
			break;
		default:
			t->logical_type = duckdb_struct_type_child_type(parent, i);
			break;
	}
	t->owns_logical_type = true;
	t->type = duckdb_get_type_id(t->logical_type);
	pdo_duckdb_appender_desc_fill(t);
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
#ifdef HAVE_DUCKDB_V2
		case DUCKDB_TYPE_TIMESTAMP_TZ_NS: kw = "TIMESTAMPTZ_NS"; break;
#endif
		case DUCKDB_TYPE_INTERVAL: kw = "INTERVAL"; break;
		case DUCKDB_TYPE_UUID: kw = "UUID"; break;
		case DUCKDB_TYPE_BIT: kw = "BIT"; break;
		default: return false;
	}
	spprintf(out_sql, 0, "SELECT CAST(? AS %s)", kw);
	return true;
}

static duckdb_prepared_statement pdo_duckdb_appender_prepare_probe(pdo_duckdb_db_handle *H, duckdb_type tid, duckdb_logical_type lt)
{
	duckdb_prepared_statement probe = NULL;
	char *sql = NULL;

	if (!pdo_duckdb_appender_probe_sql(tid, lt, &sql)) {
		return NULL;
	}
	if (!pdo_duckdb_appender_enter(H)) {
		efree(sql);
		return NULL;
	}
	if (duckdb_prepare(H->conn, sql, &probe) != DuckDBSuccess) {
		/* Probe failure leaves validation to the native append. */
		if (probe) {
			duckdb_destroy_prepare(&probe);
		}
		pdo_duckdb_instance_unlock(H);
		efree(sql);
		return NULL;
	}
	pdo_duckdb_instance_unlock(H);
	efree(sql);
	return probe;
}

/* On failure stores an emalloc'd detail in *errp. */
static bool pdo_duckdb_appender_probe_value(pdo_duckdb_db_handle *H,
	duckdb_prepared_statement probe, zval *v, char **errp)
{
	duckdb_result res = {0};
	duckdb_state bst;

	*errp = NULL;
	if (!pdo_duckdb_appender_enter(H)) {
		return false;
	}
	switch (Z_TYPE_P(v)) {
		case IS_TRUE:
		case IS_FALSE:
			bst = duckdb_bind_boolean(probe, 1, Z_TYPE_P(v) == IS_TRUE);
			break;
		case IS_LONG:
			bst = duckdb_bind_int64(probe, 1, (int64_t)Z_LVAL_P(v));
			break;
		case IS_DOUBLE:
			bst = duckdb_bind_double(probe, 1, Z_DVAL_P(v));
			break;
		default:
			bst = duckdb_bind_varchar_length(probe, 1, Z_STRVAL_P(v), (idx_t)Z_STRLEN_P(v));
			break;
	}
	if (bst != DuckDBSuccess) {
		pdo_duckdb_instance_unlock(H);
		*errp = estrdup("unable to bind the cast probe");
		return false;
	}
	if (duckdb_execute_prepared(probe, &res) != DuckDBSuccess) {
		const char *e = duckdb_result_error(&res);
		zend_try {
			*errp = estrdup(e ? e : "cast failed");
		} zend_catch {
			duckdb_destroy_result(&res);
			pdo_duckdb_instance_unlock(H);
			zend_bailout();
		} zend_end_try();
		duckdb_destroy_result(&res);
		pdo_duckdb_instance_unlock(H);
		return false;
	}
	duckdb_destroy_result(&res);
	pdo_duckdb_instance_unlock(H);
	return true;
}

static zend_object *pdo_duckdb_appender_new(zend_class_entry *ce)
{
	pdo_duckdb_appender *a = zend_object_alloc(sizeof(pdo_duckdb_appender), ce);

	a->appender = NULL;
	a->closed = false;
	a->ncols = 0;
	a->col_types = NULL;
	a->col_type_ids = NULL;
	a->col_flags = NULL;
	a->col_desc = NULL;
	a->col_probes = NULL;
	a->col_num_bounds = NULL;
	a->col_checks = NULL;
	a->pdo = NULL;
	a->H = NULL;
	a->prev_live = NULL;
	a->next_live = NULL;
	zend_object_std_init(&a->std, ce);
	object_properties_init(&a->std, ce);
	a->std.handlers = &pdo_duckdb_appender_handlers;

	return &a->std;
}

static void pdo_duckdb_appender_link(pdo_duckdb_appender *a, pdo_duckdb_db_handle *H)
{
	a->H = H;
	a->prev_live = NULL;
	a->next_live = H->appenders;
	if (H->appenders) {
		H->appenders->prev_live = a;
	}
	H->appenders = a;
}

static void pdo_duckdb_appender_unlink(pdo_duckdb_appender *a)
{
	if (!a->H) {
		return;
	}
	if (a->prev_live) {
		a->prev_live->next_live = a->next_live;
	} else {
		a->H->appenders = a->next_live;
	}
	if (a->next_live) {
		a->next_live->prev_live = a->prev_live;
	}
	a->prev_live = NULL;
	a->next_live = NULL;
	a->H = NULL;
}

/* Closes (flushing buffered rows) and destroys the native appender and its
 * cast probes while the connection is still open. Runs from free_obj, or
 * from the driver's closer when GC or request shutdown frees the PDO object
 * first; whichever runs first unlinks the appender, and an unlinked appender
 * has no native state left. */
static void pdo_duckdb_appender_release_native(pdo_duckdb_appender *a)
{
	pdo_duckdb_db_handle *H = a->H;
	duckdb_error_data close_error = NULL;
	bool close_failed = false;
	bool destroy_failed = false;
	bool allowed = false;

	if (!H) {
		return;
	}
	if (a->appender && !a->closed) {
		allowed = pdo_duckdb_native_enter(H);
	}
	if (!allowed) {
		pdo_duckdb_instance_lock(H);
	}
	pdo_duckdb_appender_unlink(a);
	if (a->appender) {
		duckdb_appender ap = a->appender;
		bool was_closed = a->closed;
		duckdb_state close_state = DuckDBSuccess;

		a->appender = NULL;
		a->closed = true;
		if (!was_closed && allowed) {
			close_state = duckdb_appender_close(ap);
			if (close_state != DuckDBSuccess) {
				close_failed = true;
				close_error = duckdb_appender_error_data(ap);
			}
		}
		/* Destroy closes again, so failed or sandbox-denied writes must be
		 * discarded first. */
		(void)duckdb_appender_clear(ap);
		if (duckdb_appender_destroy(&ap) != DuckDBSuccess && !was_closed && close_state == DuckDBSuccess) {
			destroy_failed = true;
		}
	}
	if (a->col_probes) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			if (a->col_probes[c]) {
				duckdb_destroy_prepare(&a->col_probes[c]);
			}
		}
	}
	pdo_duckdb_instance_unlock(H);
	if (a->col_probes) {
		efree(a->col_probes);
		a->col_probes = NULL;
	}
	if (close_failed) {
		pdo_duckdb_appender_report(close_error,
			"Pdo\\Duckdb\\Appender close during destruction failed", true);
	}
	if (destroy_failed) {
		php_error_docref(NULL, E_WARNING,
			"Pdo\\Duckdb\\Appender destroy during destruction failed");
	}
}

void pdo_duckdb_appenders_release(pdo_duckdb_db_handle *H)
{
	while (H->appenders) {
		pdo_duckdb_appender_release_native(H->appenders);
	}
}

/* PDO's free_obj rolls back an open transaction before the closer runs. Rows
 * still buffered in that PDO object's appenders belong to the abandoned
 * transaction, so they are discarded rather than flushed afterwards in
 * autocommit. An appender freed before its PDO object flushed into the
 * transaction instead, and the rollback discards those rows the same way. */
void pdo_duckdb_appenders_discard_for_rollback(pdo_duckdb_db_handle *H)
{
	pdo_duckdb_appender *a;

	pdo_duckdb_instance_lock(H);
	for (a = H->appenders; a; a = a->next_live) {
		/* A linked appender holds a reference to a->pdo, and neither GC nor
		 * the shutdown sweep releases object memory before every free_obj
		 * has run, so the flags read is safe. */
		if (a->appender && !a->closed && a->pdo && (OBJ_FLAGS(a->pdo) & IS_OBJ_FREE_CALLED)) {
			(void)duckdb_appender_clear(a->appender);
		}
	}
	pdo_duckdb_instance_unlock(H);
}

static void pdo_duckdb_appender_free(zend_object *obj)
{
	pdo_duckdb_appender *a = pdo_duckdb_appender_from_obj(obj);

	/* Never reach the PDO object's inner handle here: GC and request shutdown
	 * may already have freed it. A still-linked a->H is live, because the
	 * driver's closer unlinks every appender before freeing H. */
	pdo_duckdb_appender_release_native(a);
	/* Descriptors borrow col_types[i], so destroy them first. */
	if (a->col_desc) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			pdo_duckdb_appender_desc_destroy(a->col_desc[c]);
		}
		efree(a->col_desc);
		a->col_desc = NULL;
	}
	if (a->col_types) {
		idx_t c;
		for (c = 0; c < a->ncols; c++) {
			if (a->col_types[c]) {
				duckdb_destroy_logical_type(&a->col_types[c]);
			}
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
	if (a->col_num_bounds) {
		efree(a->col_num_bounds);
		a->col_num_bounds = NULL;
	}
	if (a->col_checks) {
		efree(a->col_checks);
		a->col_checks = NULL;
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

/* Bounds sit strictly inside DuckDB's cast limits, so a value within them
 * cannot fail the cast whichever way DuckDB rounds it. Values outside, and
 * NaN, fall back to the exact CAST probe. */
static bool pdo_duckdb_appender_num_bounds(duckdb_type tid, duckdb_logical_type lt, double *b)
{
	switch (tid) {
		case DUCKDB_TYPE_TINYINT:   b[0] = -127.0; b[1] = 126.0; return true;
		case DUCKDB_TYPE_SMALLINT:  b[0] = -32767.0; b[1] = 32766.0; return true;
		case DUCKDB_TYPE_INTEGER:   b[0] = -2147483647.0; b[1] = 2147483646.0; return true;
		case DUCKDB_TYPE_BIGINT:    b[0] = -9.2e18; b[1] = 9.2e18; return true;
		case DUCKDB_TYPE_HUGEINT:   b[0] = -1.7e38; b[1] = 1.7e38; return true;
		case DUCKDB_TYPE_UTINYINT:  b[0] = 0.0; b[1] = 254.0; return true;
		case DUCKDB_TYPE_USMALLINT: b[0] = 0.0; b[1] = 65534.0; return true;
		case DUCKDB_TYPE_UINTEGER:  b[0] = 0.0; b[1] = 4294967294.0; return true;
		case DUCKDB_TYPE_UBIGINT:   b[0] = 0.0; b[1] = 1.8e19; return true;
		case DUCKDB_TYPE_UHUGEINT:  b[0] = 0.0; b[1] = 3.4e38; return true;
		case DUCKDB_TYPE_DECIMAL: {
			/* Keep at least one unit of the last digit, and a relative margin
			 * far above double rounding error, below 10^width. */
			double limit = pow(10.0, (double)duckdb_decimal_width(lt));
			double margin = limit * 1e-6 > 1.0 ? limit * 1e-6 : 1.0;
			b[1] = (limit - margin) / pow(10.0, (double)duckdb_decimal_scale(lt));
			b[0] = -b[1];
			return true;
		}
		default:
			return false;
	}
}

/* A failed native append invalidates the appender and discards every row it
 * has buffered, so each scalar DuckDB might refuse to cast is checked first.
 * Planned once per column to keep appendRow() to a table lookup. */
static void pdo_duckdb_appender_plan_checks(pdo_duckdb_appender *a, idx_t c)
{
	unsigned char *chk = &a->col_checks[c * PDO_DUCKDB_CHK_KINDS];
	duckdb_type tid = a->col_type_ids[c];
	bool probe = a->col_probes[c] != NULL;
	bool bounded = probe && pdo_duckdb_appender_num_bounds(tid, a->col_types[c],
		&a->col_num_bounds[c * 2]);
	unsigned char other = probe ? PDO_DUCKDB_CHK_PROBE : PDO_DUCKDB_CHK_NONE;
	unsigned char *b = &chk[0], *l = &chk[IS_LONG - IS_FALSE];
	unsigned char *d = &chk[IS_DOUBLE - IS_FALSE], *s = &chk[IS_STRING - IS_FALSE];

	if (a->col_flags[c] & PDO_DUCKDB_APPENDER_COL_NESTED) {
		memset(chk, PDO_DUCKDB_CHK_NESTED, PDO_DUCKDB_CHK_KINDS);
		return;
	}
	if (a->col_flags[c] & PDO_DUCKDB_APPENDER_COL_BLOB) {
		memset(chk, PDO_DUCKDB_CHK_NOT_STRING, PDO_DUCKDB_CHK_KINDS);
		*s = PDO_DUCKDB_CHK_NONE;
		return;
	}
	*s = other;
	switch (tid) {
		/* Non-string scalars always cast to these targets. */
		case DUCKDB_TYPE_BOOLEAN:
		case DUCKDB_TYPE_BIT:
		case DUCKDB_TYPE_DOUBLE:
			*b = *l = *d = PDO_DUCKDB_CHK_NONE;
			break;
		case DUCKDB_TYPE_FLOAT:
			*b = *l = PDO_DUCKDB_CHK_NONE;
			*d = PDO_DUCKDB_CHK_FLOAT;
			break;
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_HUGEINT:
			*l = PDO_DUCKDB_CHK_NONE;
			*b = *d = bounded ? PDO_DUCKDB_CHK_BOUNDS : other;
			break;
		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER:
		case DUCKDB_TYPE_UBIGINT:
		case DUCKDB_TYPE_UHUGEINT:
			*l = PDO_DUCKDB_CHK_INT_RANGE;
			*b = *d = bounded ? PDO_DUCKDB_CHK_BOUNDS : other;
			break;
		default:
			*b = *l = *d = bounded ? PDO_DUCKDB_CHK_BOUNDS : other;
			break;
	}
	chk[IS_TRUE - IS_FALSE] = *b;
}

/* {{{ duckdbAppender(string $table, string|array|null $schema = null, ?array $columns = null): Pdo\Duckdb\Appender */
static void pdo_duckdb_appender_create_impl(INTERNAL_FUNCTION_PARAMETERS)
{
	zend_string *table;
	zend_string *schema = NULL;
	HashTable *schema_path = NULL;
	HashTable *columns = NULL;
	pdd_bridge_bytes *segments = NULL;
	pdd_bridge_error bridge_error = {0};
	size_t schema_count = 0;
	pdo_dbh_t *dbh;
	pdo_duckdb_db_handle *H;
	duckdb_appender ap = NULL;
	duckdb_state create_state;
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_STR(table)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_STR_OR_NULL(schema_path, schema)
		Z_PARAM_ARRAY_HT_OR_NULL(columns)
	ZEND_PARSE_PARAMETERS_END();

	dbh = Z_PDO_DBH_P(ZEND_THIS);
	PDO_CONSTRUCT_CHECK;
	H = (pdo_duckdb_db_handle *)dbh->driver_data;

	if (!pdo_duckdb_enforce_sandbox(H)) {
		zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
			"PDO::duckdbAppender(): %s", pdo_duckdb_sandbox_error(H));
		RETURN_THROWS();
	}

	/* duckdb_appender_create() would truncate "safe\0bad" to "safe". */
	if (zend_str_has_nul_byte(table) || (schema && zend_str_has_nul_byte(schema))) {
		zend_value_error("Pdo\\Duckdb\\Appender table and schema names must not contain a NUL byte");
		RETURN_THROWS();
	}
	if (schema_path) {
		zval *segment;
		schema_count = zend_hash_num_elements(schema_path);
		if (!schema_count || !zend_array_is_list(schema_path)) {
			zend_value_error("Pdo\\Duckdb\\Appender schema path must be a nonempty list of identifier segments");
			RETURN_THROWS();
		}
		ZEND_HASH_FOREACH_VAL(schema_path, segment) {
			ZVAL_DEREF(segment);
			if (Z_TYPE_P(segment) != IS_STRING) {
				zend_type_error("Pdo\\Duckdb\\Appender schema path segments must be strings");
				RETURN_THROWS();
			}
			if (!Z_STRLEN_P(segment) || zend_str_has_nul_byte(Z_STR_P(segment))) {
				zend_value_error("Pdo\\Duckdb\\Appender schema path segments must be nonempty and must not contain a NUL byte");
				RETURN_THROWS();
			}
			if (schema_count == 1) {
				schema = Z_STR_P(segment);
			}
		} ZEND_HASH_FOREACH_END();
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

	/* The object comes first so a memory_limit bailout cannot strand a native
	 * appender: from here on free_obj owns everything stored in it. */
	object_init_ex(return_value, pdo_duckdb_appender_ce);
	a = pdo_duckdb_appender_from_obj(Z_OBJ_P(return_value));
	a->pdo = Z_OBJ_P(ZEND_THIS);
	GC_ADDREF(a->pdo);
	if (schema_count > 1) {
		zval *segment;
		size_t i = 0;
		segments = ecalloc(schema_count, sizeof(*segments));
		ZEND_HASH_FOREACH_VAL(schema_path, segment) {
			ZVAL_DEREF(segment);
			segments[i].data = Z_STRVAL_P(segment);
			segments[i++].length = Z_STRLEN_P(segment);
		} ZEND_HASH_FOREACH_END();
	}

	if (!pdo_duckdb_appender_enter(H)) {
		if (segments) efree(segments);
		zval_ptr_dtor(return_value);
		ZVAL_NULL(return_value);
		RETURN_THROWS();
	}
	if (segments) {
		pdd_bridge_bytes table_name = {ZSTR_VAL(table), ZSTR_LEN(table)};
		create_state = pdd_bridge_appender_create(H->conn, segments, schema_count, table_name, &ap, &bridge_error)
			== PDD_BRIDGE_OK ? DuckDBSuccess : DuckDBError;
		efree(segments);
	} else {
		create_state = duckdb_appender_create(H->conn, schema ? ZSTR_VAL(schema) : NULL, ZSTR_VAL(table), &ap);
	}
	if (create_state != DuckDBSuccess) {
		duckdb_error_data ed = ap ? duckdb_appender_error_data(ap) : NULL;

		if (ap) {
			duckdb_appender_destroy(&ap);
		}
		pdo_duckdb_instance_unlock(H);
		if (bridge_error.data) {
			if (ed) duckdb_destroy_error_data(&ed);
			pdo_duckdb_appender_bridge_error(&bridge_error);
		} else {
			pdo_duckdb_appender_report(ed, "Unable to create DuckDB appender", false);
		}
		zval_ptr_dtor(return_value);
		ZVAL_NULL(return_value);
		RETURN_THROWS();
	}
	a->appender = ap;
	a->closed = false;
	pdo_duckdb_appender_link(a, H);

	if (columns) {
		zval *col;

		ZEND_HASH_FOREACH_VAL(columns, col) {
			ZVAL_DEREF(col);
			if (duckdb_appender_add_column(ap, Z_STRVAL_P(col)) != DuckDBSuccess) {
				/* Nothing was appended; skip the destruction-time close. */
				a->closed = true;
				pdo_duckdb_instance_unlock(H);
				pdo_duckdb_appender_throw(H, ap, "Unable to add appender column");
				zval_ptr_dtor(return_value);
				ZVAL_NULL(return_value);
				RETURN_THROWS();
			}
		} ZEND_HASH_FOREACH_END();
	}

	/* Cached so appendRow() doesn't allocate a logical type per cell. Zeroed:
	 * free_obj walks ncols entries, and a memory_limit bailout in the
	 * allocations below fires before the fill loop. */
	a->ncols = duckdb_appender_column_count(ap);
	pdo_duckdb_instance_unlock(H);
	if (a->ncols) {
		idx_t c;
		a->col_types = ecalloc(a->ncols, sizeof(duckdb_logical_type));
		a->col_type_ids = emalloc(sizeof(duckdb_type) * a->ncols);
		a->col_flags = ecalloc(a->ncols, sizeof(unsigned char));
		a->col_num_bounds = ecalloc(a->ncols * 2, sizeof(double));
		for (c = 0; c < a->ncols; c++) {
			duckdb_type tid;

			pdo_duckdb_instance_lock(H);
			a->col_types[c] = duckdb_appender_column_type(ap, c);
			pdo_duckdb_instance_unlock(H);
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
			pdo_duckdb_nested_render_type *d = pdo_duckdb_appender_desc_node(&a->col_desc[c]);
			d->type = a->col_type_ids[c];
			d->logical_type = a->col_types[c];
			pdo_duckdb_appender_desc_fill(d);
			if (EG(exception)) {
				zval_ptr_dtor(return_value);
				ZVAL_NULL(return_value);
				RETURN_THROWS();
			}
			if (!(a->col_flags[c] & PDO_DUCKDB_APPENDER_COL_NESTED)) {
				a->col_probes[c] = pdo_duckdb_appender_prepare_probe(
					H, a->col_type_ids[c], a->col_types[c]);
				if (EG(exception)) {
					zval_ptr_dtor(return_value);
					ZVAL_NULL(return_value);
					RETURN_THROWS();
				}
			}
		}
		a->col_checks = emalloc(a->ncols * PDO_DUCKDB_CHK_KINDS);
		for (c = 0; c < a->ncols; c++) {
			pdo_duckdb_appender_plan_checks(a, c);
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

	if (!a->appender || a->closed) {
		zend_throw_error(NULL, "Pdo\\Duckdb\\Appender is closed");
		return NULL;
	}
	if (a->H && !pdo_duckdb_enforce_sandbox(a->H)) {
		zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
			"Pdo\\Duckdb\\Appender: %s", pdo_duckdb_sandbox_error(a->H));
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

/* Constructed integer values bypass DuckDB's cast validation: range-check
 * before narrowing to avoid silent wraparound. */
static duckdb_value pdo_duckdb_make_leaf(zval *z, duckdb_type tid, uint32_t argpos)
{
	switch (Z_TYPE_P(z)) {
		case IS_NULL:  return duckdb_create_null_value();
		case IS_TRUE:  return duckdb_create_bool(true);
		case IS_FALSE: return duckdb_create_bool(false);
		case IS_DOUBLE:
			/* Let DuckDB validate narrowing casts instead of turning a finite
			 * out-of-range double into FLOAT infinity in C. */
			return duckdb_create_double(Z_DVAL_P(z));
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

typedef struct {
	HashTable *active;
	duckdb_value value;
	duckdb_value *children;
	pdd_bridge_bytes *keys;
	zend_string **numeric_keys;
	idx_t count;
} pdo_duckdb_variant_input_frame;

typedef struct {
	pdo_duckdb_variant_input_frame frames[PDO_DUCKDB_APPENDER_MAX_DEPTH + 1];
	pdd_bridge_error error;
} pdo_duckdb_variant_input;

static void pdo_duckdb_variant_input_clear(pdo_duckdb_variant_input_frame *frame)
{
	idx_t i;
	if (frame->value) {
		duckdb_destroy_value(&frame->value);
	}
	for (i = 0; i < frame->count; i++) {
		if (frame->children && frame->children[i]) {
			duckdb_destroy_value(&frame->children[i]);
		}
		if (frame->numeric_keys && frame->numeric_keys[i]) {
			zend_string_release(frame->numeric_keys[i]);
		}
	}
	if (frame->children) efree(frame->children);
	if (frame->keys) efree(frame->keys);
	if (frame->numeric_keys) efree(frame->numeric_keys);
	memset(frame, 0, sizeof(*frame));
}

static duckdb_value pdo_duckdb_variant_input_build(pdo_duckdb_variant_input *input, zval *z,
	uint32_t argpos, int depth)
{
	pdo_duckdb_variant_input_frame *frame;
	HashTable *ht;
	bool object;
	zend_string *key;
	zend_ulong index;
	zval *child;
	idx_t i = 0;
	duckdb_value result;

	if (depth > PDO_DUCKDB_APPENDER_MAX_DEPTH) {
		zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): maximum nesting depth (128) exceeded");
		return NULL;
	}
	frame = &input->frames[depth];
	ZVAL_DEREF(z);
	if (Z_TYPE_P(z) != IS_ARRAY && Z_TYPE_P(z) != IS_OBJECT) {
		frame->value = pdo_duckdb_make_leaf(z, DUCKDB_TYPE_VARIANT, argpos);
		if (!frame->value && !EG(exception)) {
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u contains an unsupported VARIANT value or invalid UTF-8 string",
				argpos);
		}
		result = frame->value;
		frame->value = NULL;
		return result;
	}
	if (Z_TYPE_P(z) == IS_OBJECT) {
		if (Z_OBJCE_P(z) != zend_standard_class_def) {
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects plain stdClass objects for VARIANT objects",
				argpos);
			return NULL;
		}
		ht = zend_std_get_properties(Z_OBJ_P(z));
		object = true;
	} else {
		ht = Z_ARRVAL_P(z);
		object = !zend_array_is_list(ht);
	}
	for (int ancestor = 0; ancestor < depth; ancestor++) {
		if (input->frames[ancestor].active == ht) {
			zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u contains a recursive VARIANT value", argpos);
			return NULL;
		}
	}
	frame->active = ht;
	frame->count = zend_hash_num_elements(ht);
	frame->children = ecalloc(frame->count ? frame->count : 1, sizeof(*frame->children));
	if (object) {
		frame->keys = ecalloc(frame->count ? frame->count : 1, sizeof(*frame->keys));
		frame->numeric_keys = ecalloc(frame->count ? frame->count : 1, sizeof(*frame->numeric_keys));
	}
	ZEND_HASH_FOREACH_KEY_VAL(ht, index, key, child) {
		if (object) {
			if (!key) {
				frame->numeric_keys[i] = zend_long_to_str((zend_long)index);
				key = frame->numeric_keys[i];
			}
			frame->keys[i].data = ZSTR_VAL(key);
			frame->keys[i].length = ZSTR_LEN(key);
		}
		frame->children[i] = pdo_duckdb_variant_input_build(input, child, argpos, depth + 1);
		if (!frame->children[i]) {
			return NULL;
		}
		i++;
	} ZEND_HASH_FOREACH_END();
	if ((object ? pdd_bridge_variant_object(frame->keys, frame->children, frame->count, &frame->value, &input->error)
		: pdd_bridge_variant_array(frame->children, frame->count, &frame->value, &input->error)) != PDD_BRIDGE_OK) {
		pdo_duckdb_appender_bridge_error(&input->error);
		return NULL;
	}
	result = frame->value;
	frame->value = NULL;
	pdo_duckdb_variant_input_clear(frame);
	return result;
}

static duckdb_value pdo_duckdb_build_variant(zval *z, uint32_t argpos, int depth)
{
	pdo_duckdb_variant_input *input = ecalloc(1, sizeof(*input));
	duckdb_value result = NULL;

	zend_try {
		result = pdo_duckdb_variant_input_build(input, z, argpos, depth);
	} zend_catch {
		for (int i = 0; i <= PDO_DUCKDB_APPENDER_MAX_DEPTH; i++) {
			pdo_duckdb_variant_input_clear(&input->frames[i]);
		}
		pdd_bridge_error_free(&input->error);
		efree(input);
		zend_bailout();
	} zend_end_try();
	for (int i = 0; i <= PDO_DUCKDB_APPENDER_MAX_DEPTH; i++) {
		pdo_duckdb_variant_input_clear(&input->frames[i]);
	}
	pdd_bridge_error_free(&input->error);
	efree(input);
	return result;
}

typedef struct {
	duckdb_value *keys;
	duckdb_value *values;
	idx_t count;
} pdo_duckdb_cell_input;

static duckdb_value pdo_duckdb_build_value(zval *z, const pdo_duckdb_nested_render_type *t,
	uint32_t argpos, int depth);

static void pdo_duckdb_cell_input_free(pdo_duckdb_cell_input *input)
{
	for (idx_t i = 0; i < input->count; i++) {
		if (input->keys && input->keys[i]) duckdb_destroy_value(&input->keys[i]);
		if (input->values && input->values[i]) duckdb_destroy_value(&input->values[i]);
	}
	if (input->keys) efree(input->keys);
	if (input->values) efree(input->values);
	efree(input);
}

/* depth starts at zero; argpos is the 1-based appendRow() argument number. */
static duckdb_value pdo_duckdb_build_value_impl(zval *z, const pdo_duckdb_nested_render_type *t,
	uint32_t argpos, int depth, pdo_duckdb_cell_input *input)
{
	duckdb_type tid = t->type;
	HashTable *ht;

	if (depth > PDO_DUCKDB_APPENDER_MAX_DEPTH) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"Pdo\\Duckdb\\Appender::appendRow(): maximum nesting depth (128) exceeded");
		return NULL;
	}

	ZVAL_DEREF(z);
	if (tid == DUCKDB_TYPE_VARIANT && (Z_TYPE_P(z) == IS_ARRAY || Z_TYPE_P(z) == IS_OBJECT)) {
		return pdo_duckdb_build_variant(z, argpos, depth);
	}

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
			 * unsupported type when it returned NULL without throwing.
			 * duckdb_create_varchar_length returns NULL for invalid UTF-8. */
			if (Z_TYPE_P(z) == IS_STRING && tid != DUCKDB_TYPE_BLOB) {
				zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u contains a string that is not valid UTF-8",
					argpos);
			} else {
				zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is of unsupported type %s",
					argpos, zend_zval_value_name(z));
			}
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
			input->count = n;
			duckdb_value *vals = input->values = ecalloc(n ? n : 1, sizeof(duckdb_value));
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
			return ret;
		}

		case DUCKDB_TYPE_STRUCT: {
			const pdo_duckdb_appender_desc *desc = (const pdo_duckdb_appender_desc *)t;
			idx_t cnt = t->child_count;
			duckdb_value *vals;
			idx_t i, built = 0;
			duckdb_value ret = NULL;
			bool ok = true;

			if (desc->tuple && !zend_array_is_list(ht)) {
				zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects a list-shaped array for a tuple",
					argpos);
				return NULL;
			}
			if (zend_hash_num_elements(ht) != cnt) {
				zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects exactly %u struct field(s), got %u",
					argpos, (uint32_t)cnt, (uint32_t)zend_hash_num_elements(ht));
				return NULL;
			}
			input->count = cnt;
			vals = input->values = ecalloc(cnt ? cnt : 1, sizeof(duckdb_value));

			for (i = 0; i < cnt; i++) {
				const pdd_bridge_bytes *name = &desc->names[i];
				zval *fv = desc->tuple ? zend_hash_index_find(ht, i)
					: zend_symtable_str_find(ht, name->data, name->length);
				duckdb_value cv;

				if (!fv) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is missing struct field \"%s\"",
						argpos, name->data ? name->data : "");
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
			return ret;
		}

		case DUCKDB_TYPE_MAP: {
			idx_t n = zend_hash_num_elements(ht);
			input->count = n;
			duckdb_value *keys = input->keys = ecalloc(n ? n : 1, sizeof(duckdb_value));
			duckdb_value *vals = input->values = ecalloc(n ? n : 1, sizeof(duckdb_value));
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
				kv = keys[built] = pdo_duckdb_build_value(&ztmp, t->children[0], argpos, depth + 1);
				if (!kv) { ok = false; break; }
				vv = vals[built] = pdo_duckdb_build_value(val, t->children[1], argpos, depth + 1);
				if (!vv) { ok = false; break; }
				built++;
			} ZEND_HASH_FOREACH_END();

			if (ok) {
				ret = duckdb_create_map_value(t->logical_type, keys, vals, n);
				if (!ret) {
					zend_value_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u could not be built as a map", argpos);
				}
			}
			return ret;
		}

		default:
			zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is an array but the target column is not a nested type", argpos);
			return NULL;
	}
}

static duckdb_value pdo_duckdb_build_value(zval *z, const pdo_duckdb_nested_render_type *t,
	uint32_t argpos, int depth)
{
	pdo_duckdb_cell_input *input;
	duckdb_value value = NULL;
	zval *const dereferenced = Z_ISREF_P(z) ? Z_REFVAL_P(z) : z;

	if (Z_TYPE_P(dereferenced) != IS_ARRAY || t->type == DUCKDB_TYPE_VARIANT) {
		return pdo_duckdb_build_value_impl(z, t, argpos, depth, NULL);
	}
	input = ecalloc(1, sizeof(*input));
	zend_try {
		value = pdo_duckdb_build_value_impl(z, t, argpos, depth, input);
	} zend_catch {
		pdo_duckdb_cell_input_free(input);
		zend_bailout();
	} zend_end_try();
	pdo_duckdb_cell_input_free(input);
	return value;
}

/* Smallest magnitude that rounds to FLOAT infinity (2^128 - 2^103), which is
 * where DuckDB's DOUBLE -> FLOAT cast starts failing. Comparing avoids the
 * undefined out-of-range double -> float conversion in C. */
#define PDO_DUCKDB_FLOAT_ROUNDS_TO_INF 0x1.ffffffp+127

static bool pdo_duckdb_appender_probe_row_value(pdo_duckdb_appender *a, uint32_t i, zval *v)
{
	char *perr = NULL;

	if (pdo_duckdb_appender_probe_value(a->H, a->col_probes[i], v, &perr)) {
		return true;
	}
	if (!EG(exception)) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"Failed to append value: %s", perr ? perr : "unknown error");
	}
	if (perr) {
		efree(perr);
	}
	return false;
}

static void pdo_duckdb_appender_row_values_free(duckdb_value *built, uint32_t count)
{
	if (built) {
		for (uint32_t i = 0; i < count; i++) {
			if (built[i]) duckdb_destroy_value(&built[i]);
		}
		efree(built);
	}
}

static bool pdo_duckdb_appender_append_row(pdo_duckdb_appender *a, zval *args, uint32_t argc,
	duckdb_value *volatile *built_out)
{
	duckdb_value *built = NULL;
	uint32_t i;

	for (i = 0; i < argc; i++) {
		zval *v = &args[i];
		ZVAL_DEREF(v);
		switch (Z_TYPE_P(v)) {
			case IS_TRUE:
			case IS_FALSE:
			case IS_LONG:
			case IS_DOUBLE:
			case IS_STRING: {
				double d;
				switch (a->col_checks[i * PDO_DUCKDB_CHK_KINDS + (Z_TYPE_P(v) - IS_FALSE)]) {
					case PDO_DUCKDB_CHK_NONE:
						break;
					case PDO_DUCKDB_CHK_INT_RANGE:
						if (!pdo_duckdb_validate_integer_range(Z_LVAL_P(v), a->col_type_ids[i], i + 1)) {
							return false;
						}
						break;
					case PDO_DUCKDB_CHK_FLOAT:
						if (zend_finite(Z_DVAL_P(v)) &&
							fabs(Z_DVAL_P(v)) >= PDO_DUCKDB_FLOAT_ROUNDS_TO_INF) {
							zend_throw_exception_ex(php_pdo_get_exception(), 0,
								"Failed to append value: argument #%u value %.17G is out of range for FLOAT",
								i + 1, Z_DVAL_P(v));
							return false;
						}
						break;
					case PDO_DUCKDB_CHK_BOUNDS:
						d = Z_TYPE_P(v) == IS_DOUBLE ? Z_DVAL_P(v)
							: Z_TYPE_P(v) == IS_LONG ? (double)Z_LVAL_P(v)
							: (double)(Z_TYPE_P(v) == IS_TRUE);
						if (d >= a->col_num_bounds[i * 2] && d <= a->col_num_bounds[i * 2 + 1]) {
							break;
						}
						if (!pdo_duckdb_appender_probe_row_value(a, i, v)) {
							return false;
						}
						break;
					case PDO_DUCKDB_CHK_PROBE:
						if (!pdo_duckdb_appender_probe_row_value(a, i, v)) {
							return false;
						}
						break;
					case PDO_DUCKDB_CHK_NOT_STRING:
						zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects a string for a BLOB column",
							i + 1);
						return false;
					case PDO_DUCKDB_CHK_NESTED:
						zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u expects an array for a nested column",
							i + 1);
						return false;
				}
				break;
			}
			case IS_NULL:
				break;
			case IS_ARRAY:
			case IS_OBJECT:
				if (!built) {
					built = ecalloc(argc, sizeof(duckdb_value));
					*built_out = built;
				}
				built[i] = pdo_duckdb_build_value(v, a->col_desc[i], i + 1, 0);
				if (!built[i]) {
					return false;
				}
				break;
			default:
				zend_type_error("Pdo\\Duckdb\\Appender::appendRow(): argument #%u is of unsupported type %s",
					i + 1, zend_zval_value_name(v));
				return false;
		}
	}

	if (!pdo_duckdb_appender_enter(a->H)) {
		return false;
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
			pdo_duckdb_instance_unlock(a->H);
			pdo_duckdb_appender_throw(a->H, a->appender, "Failed to append value");
			return false;
		}
	}
	if (duckdb_appender_end_row(a->appender) != DuckDBSuccess) {
		a->closed = true;
		pdo_duckdb_instance_unlock(a->H);
		pdo_duckdb_appender_throw(a->H, a->appender, "Failed to end appender row");
		return false;
	}
	pdo_duckdb_instance_unlock(a->H);

	return true;
}

ZEND_METHOD(Pdo_Duckdb_Appender, appendRow)
{
	zval *args = NULL;
	uint32_t argc = 0;
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

	duckdb_value *volatile built = NULL;
	bool ok;

	zend_try {
		ok = pdo_duckdb_appender_append_row(a, args, argc, &built);
	} zend_catch {
		pdo_duckdb_appender_row_values_free(built, argc);
		zend_bailout();
	} zend_end_try();
	pdo_duckdb_appender_row_values_free(built, argc);
	if (!ok) {
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
	if (!pdo_duckdb_appender_enter(a->H)) {
		RETURN_THROWS();
	}
	if (duckdb_appender_flush(a->appender) != DuckDBSuccess) {
		/* DuckDB invalidates the appender after a failed flush. */
		a->closed = true;
		pdo_duckdb_instance_unlock(a->H);
		pdo_duckdb_appender_throw(a->H, a->appender, "Failed to flush appender");
		RETURN_THROWS();
	}
	pdo_duckdb_instance_unlock(a->H);
}

ZEND_METHOD(Pdo_Duckdb_Appender, close)
{
	pdo_duckdb_appender *a;

	ZEND_PARSE_PARAMETERS_NONE();

	a = pdo_duckdb_appender_live(ZEND_THIS);
	if (!a) {
		RETURN_THROWS();
	}
	if (!pdo_duckdb_appender_enter(a->H)) {
		RETURN_THROWS();
	}
	if (duckdb_appender_close(a->appender) != DuckDBSuccess) {
		a->closed = true;
		pdo_duckdb_instance_unlock(a->H);
		pdo_duckdb_appender_throw(a->H, a->appender, "Failed to close appender");
		RETURN_THROWS();
	}
	a->closed = true;
	pdo_duckdb_instance_unlock(a->H);
}
