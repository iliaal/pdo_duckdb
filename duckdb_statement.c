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
#include "ext/pdo/php_pdo_error.h"
#include "zend_smart_str.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include <inttypes.h>

#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
# define PDO_DUCKDB_HAVE_INT128 1
#else
# define PDO_DUCKDB_HAVE_INT128 0
#endif

static pdo_duckdb_nested_render_type *pdo_duckdb_nested_render_type_build(
	duckdb_type tid, duckdb_logical_type lt, bool owns_lt);
static void pdo_duckdb_nested_render_type_destroy(pdo_duckdb_nested_render_type *type);
/* LOB cap (CR-005): PARAM_LOB streams larger than this fail the bind. */
#define PDO_DUCKDB_LOB_MAX_BYTES ((size_t)67108864)

/* Per-chunk column cache (CR-001). fetch() (re)loads it whenever a chunk is
 * pulled; get_col() indexes it instead of calling
 * duckdb_data_chunk_get_column_count/get_vector and
 * duckdb_vector_get_validity/get_data per cell. Thread-local, so statements
 * on different threads never share it. The entry is published for a chunk
 * only after every slot is filled, and invalidated whenever that chunk is
 * destroyed (DuckDB may reuse the address for a later chunk). Buffers are
 * malloc'd (they outlive a request) and grow-only. Callers always revalidate
 * the entry against S->chunk before indexing it, and fetch() repopulates on
 * every load, so a stale entry can only trigger a best-effort refresh, never
 * a wrong read. */
typedef struct {
	duckdb_data_chunk chunk;	/* identity key; NULL when empty */
	idx_t ncols;			/* column count at load time */
	idx_t cap;			/* allocated slots */
	void *blob;			/* single buffer holding all three arrays */
	duckdb_vector *vecs;
	void **datas;
	uint64_t **validities;
} pdo_duckdb_chunk_cache;

ZEND_TLS pdo_duckdb_chunk_cache pdo_duckdb_tls_chunk_cache;

static void pdo_duckdb_chunk_cache_invalidate(duckdb_data_chunk chunk)
{
	if (chunk && pdo_duckdb_tls_chunk_cache.chunk == chunk) {
		pdo_duckdb_tls_chunk_cache.chunk = NULL;
	}
}

static bool pdo_duckdb_chunk_cache_load(pdo_duckdb_stmt *S)
{
	pdo_duckdb_chunk_cache *c = &pdo_duckdb_tls_chunk_cache;
	size_t stride = sizeof(duckdb_vector) + sizeof(void *) + sizeof(uint64_t *);
	idx_t n, i;

	c->chunk = NULL;
	c->ncols = 0;
	if (!S->chunk) {
		return false;
	}
	n = duckdb_data_chunk_get_column_count(S->chunk);
	if (n > c->cap) {
		void *blob;

		if ((uint64_t)n > (uint64_t)SIZE_MAX / (uint64_t)stride) {
			return false;
		}
		blob = realloc(c->blob, (size_t)n * stride);
		if (!blob) {
			return false;
		}
		c->blob = blob;
		c->cap = n;
		c->vecs = (duckdb_vector *)blob;
		c->datas = (void **)((char *)blob + (size_t)n * sizeof(duckdb_vector));
		c->validities = (uint64_t **)((char *)blob
			+ (size_t)n * (sizeof(duckdb_vector) + sizeof(void *)));
	}
	for (i = 0; i < n; i++) {
		duckdb_vector v = duckdb_data_chunk_get_vector(S->chunk, i);

		c->vecs[i] = v;
		c->datas[i] = duckdb_vector_get_data(v);
		c->validities[i] = duckdb_vector_get_validity(v);
	}
	c->ncols = n;
	c->chunk = S->chunk;
	return true;
}

/* Per-result-column scalar cache (CR-009). Built in cache_columns() at
 * execute time from the already-cached logical types: DECIMAL width, scale
 * and internal type plus interned ENUM dictionary strings, so per-cell
 * fetches skip the per-cell decimal/enum metadata calls and the per-cell
 * duckdb_enum_dictionary_value malloc/free. Single entry keyed by owning
 * statement; a statement whose entry was evicted (or never built) falls back
 * to the generic logical-type path with identical output. ENUM strings are
 * malloc'd copies, never zend_string: the entry outlives a request. */
typedef struct {
	bool is_decimal;
	uint8_t dec_width;
	uint8_t dec_scale;
	duckdb_type dec_internal;
	bool is_enum;
	duckdb_type enum_internal;
	char **enum_vals;		/* malloc'd copies, enum_size slots */
	size_t *enum_lens;
	uint64_t enum_size;
} pdo_duckdb_col_aux;

typedef struct {
	pdo_duckdb_stmt *owner;	/* statement the entry was built for */
	idx_t ncols;
	idx_t cap;
	pdo_duckdb_col_aux *cols;	/* malloc'd, cap slots */
} pdo_duckdb_col_aux_cache;

ZEND_TLS pdo_duckdb_col_aux_cache pdo_duckdb_tls_col_aux;

static void pdo_duckdb_col_aux_free_content(void)
{
	pdo_duckdb_col_aux_cache *a = &pdo_duckdb_tls_col_aux;
	idx_t c;

	if (!a->cols) {
		return;
	}
	for (c = 0; c < a->ncols; c++) {
		uint64_t i;

		for (i = 0; i < a->cols[c].enum_size; i++) {
			free(a->cols[c].enum_vals[i]);
		}
		free(a->cols[c].enum_vals);
		free(a->cols[c].enum_lens);
		a->cols[c].enum_vals = NULL;
		a->cols[c].enum_lens = NULL;
		a->cols[c].enum_size = 0;
		a->cols[c].is_decimal = false;
		a->cols[c].is_enum = false;
	}
}

/* Release the entry when its owner resets (execute/dtor/cursor close). Kept
 * as a buffer for reuse; only the content is freed. */
static void pdo_duckdb_col_aux_release(pdo_duckdb_stmt *S)
{
	pdo_duckdb_col_aux_cache *a = &pdo_duckdb_tls_col_aux;

	if (a->owner == S) {
		pdo_duckdb_col_aux_free_content();
		a->owner = NULL;
		a->ncols = 0;
	}
}

static pdo_duckdb_col_aux *pdo_duckdb_col_aux_for(pdo_duckdb_stmt *S, idx_t colno)
{
	pdo_duckdb_col_aux_cache *a = &pdo_duckdb_tls_col_aux;
	pdo_duckdb_col_aux *aux;

	if (a->owner != S || !a->cols || colno >= a->ncols) {
		return NULL;
	}
	aux = &a->cols[colno];
	if (!aux->is_decimal && !aux->is_enum) {
		return NULL;
	}
	return aux;
}

/* Request/process shutdown cleanup for the TLS caches above. The index
 * buffers are grow-only and malloc'd (they outlive a request), so without
 * this they are still reachable at thread exit and trip LSan. Safe to call
 * any time: every user revalidates (chunk key, aux owner) and falls back to
 * direct reads/generic rendering on a miss, and a later request simply
 * regrows the buffers. Wired by the module RSHUTDOWN/MSHUTDOWN. */
void pdo_duckdb_tls_caches_shutdown(void)
{
	pdo_duckdb_chunk_cache *c = &pdo_duckdb_tls_chunk_cache;
	pdo_duckdb_col_aux_cache *a = &pdo_duckdb_tls_col_aux;

	pdo_duckdb_col_aux_free_content();
	free(a->cols);
	a->cols = NULL;
	a->cap = 0;
	a->ncols = 0;
	a->owner = NULL;

	free(c->blob);
	c->blob = NULL;
	c->vecs = NULL;
	c->datas = NULL;
	c->validities = NULL;
	c->cap = 0;
	c->ncols = 0;
	c->chunk = NULL;
}

static void pdo_duckdb_stmt_cache_col_aux(pdo_duckdb_stmt *S);

static void pdo_duckdb_stmt_reset_result(pdo_duckdb_stmt *S)
{
	if (S->col_nested_renderers) {
		idx_t c;
		for (c = 0; c < S->col_count; c++) {
			pdo_duckdb_nested_render_type_destroy(S->col_nested_renderers[c]);
		}
		efree(S->col_nested_renderers);
		S->col_nested_renderers = NULL;
	}
	if (S->col_logical_types) {
		idx_t c;
		for (c = 0; c < S->col_count; c++) {
			duckdb_destroy_logical_type(&S->col_logical_types[c]);
		}
		efree(S->col_logical_types);
		S->col_logical_types = NULL;
	}
	if (S->col_types) {
		efree(S->col_types);
		S->col_types = NULL;
	}
	S->col_count = 0;
	pdo_duckdb_col_aux_release(S);

	if (S->chunk) {
		pdo_duckdb_chunk_cache_invalidate(S->chunk);
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

static void pdo_duckdb_stmt_reset_result_full(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt_reset_result((pdo_duckdb_stmt *)stmt->driver_data);
	php_pdo_stmt_set_column_count(stmt, 0);
}

static zend_long pdo_duckdb_stmt_rows_changed(duckdb_result *result)
{
	return duckdb_result_return_type(*result) == DUCKDB_RESULT_TYPE_CHANGED_ROWS
		? (zend_long)duckdb_rows_changed(result)
		: 0;
}

static void pdo_duckdb_stmt_cache_columns(pdo_duckdb_stmt *S)
{
	idx_t c;

	S->col_count = duckdb_column_count(&S->result);
	if (S->col_count == 0) {
		return;
	}

	S->col_types = emalloc(sizeof(duckdb_type) * S->col_count);
	S->col_logical_types = emalloc(sizeof(duckdb_logical_type) * S->col_count);
	S->col_nested_renderers = ecalloc(S->col_count, sizeof(pdo_duckdb_nested_render_type *));
	for (c = 0; c < S->col_count; c++) {
		S->col_types[c] = duckdb_column_type(&S->result, c);
		S->col_logical_types[c] = duckdb_column_logical_type(&S->result, c);
		switch (S->col_types[c]) {
			case DUCKDB_TYPE_LIST:
			case DUCKDB_TYPE_ARRAY:
			case DUCKDB_TYPE_STRUCT:
			case DUCKDB_TYPE_MAP:
			case DUCKDB_TYPE_UNION:
				S->col_nested_renderers[c] = pdo_duckdb_nested_render_type_build(
					S->col_types[c], S->col_logical_types[c], false);
				break;
			default:
				break;
		}
	}
	pdo_duckdb_stmt_cache_col_aux(S);
}

static int pdo_duckdb_stmt_dtor(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	pdo_duckdb_stmt_reset_result(S);
	if (S->prepared) {
		duckdb_destroy_prepare(&S->prepared);
		S->prepared = NULL;
	}
	pdo_duckdb_clear_einfo(&S->einfo, false);
	efree(S);
	return 1;
}

static int pdo_duckdb_stmt_execute(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	pdo_duckdb_stmt_reset_result_full(stmt);
	/* Zero before any failure return so re-execute errors do not leave a stale rowCount. */
	stmt->row_count = 0;

	/* open_basedir may have been tightened after this statement was prepared;
	 * apply the sandbox to the connection before re-executing. EXEC_PRE may
	 * already have set binds_cleared; reset the latch on failure so the next
	 * execute does not skip duckdb_clear_bindings. */
	if (!pdo_duckdb_enforce_sandbox(S->H)) {
		S->binds_cleared = false;
		pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_SANDBOX, "Unable to apply the open_basedir sandbox profile to DuckDB");
		return 0;
	}

	/* If no parameter was bound this round (execute([]) or a params-less
	 * re-execute), the first-EXEC_PRE clear never ran, so drop any bindings left
	 * from a prior execute here. Then arm the latch for the next round. */
	if (!S->binds_cleared) {
		duckdb_clear_bindings(S->prepared);
	}
	S->binds_cleared = false;

	if (S->H->unbuffered) {
		/* Opt-in streaming (PDO::DUCKDB_ATTR_UNBUFFERED): the pending-result API
		 * yields a streaming result that produces chunks lazily as fetch_chunk()
		 * pulls them, so a huge SELECT isn't buffered whole. The driver does not
		 * impose a single-active-stream guard; interleaved unbuffered statements
		 * are allowed (tests/043). closeCursor() releases a stream early. */
		duckdb_pending_result pending = NULL;

		if (duckdb_pending_prepared_streaming(S->prepared, &pending) != DuckDBSuccess) {
			pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_STREAMING, pending ? duckdb_pending_error(pending) : "Unable to create streaming result");
			duckdb_destroy_pending(&pending);
			return 0;
		}
		if (duckdb_execute_pending(pending, &S->result) != DuckDBSuccess) {
			pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_STREAMING, duckdb_result_error(&S->result));
			duckdb_destroy_result(&S->result);
			duckdb_destroy_pending(&pending);
			return 0;
		}
		duckdb_destroy_pending(&pending);
	} else {
		/* duckdb_execute_prepared returns a *materialized* result: DuckDB buffers
		 * the whole result set here, and duckdb_fetch_chunk() below streams chunks
		 * out of that buffer. So fetching is chunked but memory is bounded by the
		 * full result, not row-streamed. */
		if (duckdb_execute_prepared(S->prepared, &S->result) != DuckDBSuccess) {
			pdo_duckdb_error_stmt(stmt, duckdb_result_error(&S->result));
			duckdb_destroy_result(&S->result);
			return 0;
		}
	}

	S->has_result = true;
	if (UNEXPECTED(S->transaction_effect != PDO_DUCKDB_TRANSACTION_NONE)) {
		pdo_duckdb_apply_transaction_effect(stmt->dbh, S->transaction_effect);
	}
	pdo_duckdb_stmt_cache_columns(S);
	php_pdo_stmt_set_column_count(stmt, (int)S->col_count);
	/* reset_result_full freed columns via set_column_count(0). PDO only
	 * auto-describes when !stmt->executed; clear the latch so re-execute
	 * rebuilds columns (getColumnMeta otherwise SEGVs). */
	if (S->col_count > 0) {
		stmt->executed = 0;
	}
	stmt->row_count = pdo_duckdb_stmt_rows_changed(&S->result);
	pdo_duckdb_clear_einfo(&S->einfo, false);

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

static duckdb_value pdo_duckdb_cell_to_value_typed(duckdb_vector vec, idx_t row,
	duckdb_logical_type lt, bool destroy_lt, bool *unsupported_variant);

static duckdb_value pdo_duckdb_enum_value(duckdb_logical_type lt, void *data, idx_t row)
{
	uint64_t idx;
	switch (duckdb_enum_internal_type(lt)) {
		case DUCKDB_TYPE_USMALLINT: idx = ((uint16_t *)data)[row]; break;
		case DUCKDB_TYPE_UINTEGER:  idx = ((uint32_t *)data)[row]; break;
		case DUCKDB_TYPE_UTINYINT:
		default:                    idx = ((uint8_t *)data)[row];  break;
	}
	if (idx >= duckdb_enum_dictionary_size(lt)) {
		return duckdb_create_null_value();
	}
	return duckdb_create_enum_value(lt, idx);
}

static size_t pdo_duckdb_format_u64(uint64_t value, char *out)
{
	char tmp[20];
	size_t pos = sizeof(tmp);

	do {
		tmp[--pos] = (char)('0' + (value % 10));
		value /= 10;
	} while (value);

	memcpy(out, tmp + pos, sizeof(tmp) - pos);
	return sizeof(tmp) - pos;
}

static size_t pdo_duckdb_format_u128(uint64_t upper, uint64_t lower, char *out)
{
	char tmp[40];
	size_t pos = sizeof(tmp);

	if (upper == 0) {
		return pdo_duckdb_format_u64(lower, out);
	}

#if PDO_DUCKDB_HAVE_INT128
	{
		__uint128_t value = ((__uint128_t)upper << 64) | lower;

		while (value) {
			tmp[--pos] = (char)('0' + (value % 10));
			value /= 10;
		}
	}
#else
	while (upper || lower) {
		uint64_t q_upper = 0, q_lower = 0;
		uint32_t rem = 0;
		int bit;

		for (bit = 127; bit >= 0; bit--) {
			uint32_t b = bit >= 64
				? (uint32_t)((upper >> (bit - 64)) & 1)
				: (uint32_t)((lower >> bit) & 1);
			rem = (uint32_t)((rem << 1) | b);
			if (rem >= 10) {
				rem -= 10;
				if (bit >= 64) {
					q_upper |= (uint64_t)1 << (bit - 64);
				} else {
					q_lower |= (uint64_t)1 << bit;
				}
			}
		}

		tmp[--pos] = (char)('0' + rem);
		upper = q_upper;
		lower = q_lower;
	}
#endif

	memcpy(out, tmp + pos, sizeof(tmp) - pos);
	return sizeof(tmp) - pos;
}

static zend_string *pdo_duckdb_uhugeint_to_string(duckdb_uhugeint h)
{
	char digits[40];
	size_t len = pdo_duckdb_format_u128(h.upper, h.lower, digits);
	return zend_string_init(digits, len, 0);
}

static zend_string *pdo_duckdb_hugeint_to_string(duckdb_hugeint h)
{
	char digits[40], out[41];
	uint64_t upper = (uint64_t)h.upper;
	uint64_t lower = h.lower;
	size_t len;

	if (h.upper < 0) {
		lower = ~lower + 1;
		upper = ~upper + (lower == 0 ? 1 : 0);
		len = pdo_duckdb_format_u128(upper, lower, digits);
		out[0] = '-';
		memcpy(out + 1, digits, len);
		return zend_string_init(out, len + 1, 0);
	}

	len = pdo_duckdb_format_u128(upper, lower, digits);
	return zend_string_init(digits, len, 0);
}

static bool pdo_duckdb_decimal_meta_ok(uint8_t width, uint8_t scale)
{
	/* DuckDB Decimal::IsValidWidthScale: width in [1,38], scale <= width.
	 * Deserialization and Parquet only enforce width <= 38, so a hostile
	 * scale can be any uint8. The fast renderer writes 2+scale bytes into
	 * a 43-byte stack buffer. */
	return width >= 1 && width <= 38 && scale <= width;
}

/* Build the CR-009 per-column cache from the already-cached logical types.
 * DECIMAL columns remember width/scale/internal-type (validated once here so
 * per-cell fetches do no C-API meta calls); ENUM columns intern the whole
 * dictionary (malloc'd copies freed with the entry) so per-cell fetches are
 * an index, a bounds check and a copy. Anything unexpected (bad meta,
 * unknown internal type, allocation failure) simply leaves the column
 * unflagged and the generic logical-type path renders it as before. */
static void pdo_duckdb_stmt_cache_col_aux(pdo_duckdb_stmt *S)
{
	pdo_duckdb_col_aux_cache *a = &pdo_duckdb_tls_col_aux;
	idx_t c;

	/* An entry owned by another statement is evicted here; that statement
	 * keeps working via the generic logical-type path (its own
	 * col_logical_types stay alive until its own reset). */
	pdo_duckdb_col_aux_free_content();
	a->owner = NULL;
	a->ncols = 0;
	if (S->col_count == 0) {
		return;
	}
	if (S->col_count > a->cap) {
		pdo_duckdb_col_aux *cols = realloc(a->cols, sizeof(*cols) * (size_t)S->col_count);

		if (!cols) {
			return;
		}
		a->cols = cols;
		a->cap = S->col_count;
	}
	memset(a->cols, 0, sizeof(*a->cols) * (size_t)S->col_count);
	for (c = 0; c < S->col_count; c++) {
		duckdb_type tid = S->col_types[c];
		duckdb_logical_type lt = S->col_logical_types[c];

		if (tid == DUCKDB_TYPE_DECIMAL) {
			uint8_t width = duckdb_decimal_width(lt);
			uint8_t scale = duckdb_decimal_scale(lt);
			duckdb_type internal = duckdb_decimal_internal_type(lt);

			if (pdo_duckdb_decimal_meta_ok(width, scale)
					&& (internal == DUCKDB_TYPE_SMALLINT || internal == DUCKDB_TYPE_INTEGER
						|| internal == DUCKDB_TYPE_BIGINT || internal == DUCKDB_TYPE_HUGEINT)) {
				a->cols[c].is_decimal = true;
				a->cols[c].dec_width = width;
				a->cols[c].dec_scale = scale;
				a->cols[c].dec_internal = internal;
			}
		} else if (tid == DUCKDB_TYPE_ENUM) {
			duckdb_type internal = duckdb_enum_internal_type(lt);

			if (internal == DUCKDB_TYPE_UTINYINT || internal == DUCKDB_TYPE_USMALLINT
					|| internal == DUCKDB_TYPE_UINTEGER) {
				uint64_t n = duckdb_enum_dictionary_size(lt);
				uint64_t i, j;
				char **vals;
				size_t *lens;
				bool ok = true;

				if (n > (uint64_t)SIZE_MAX / sizeof(*vals)
						|| n > (uint64_t)SIZE_MAX / sizeof(*lens)) {
					continue;
				}
				vals = n ? malloc(sizeof(*vals) * (size_t)n) : NULL;
				lens = n ? malloc(sizeof(*lens) * (size_t)n) : NULL;
				if (n && (!vals || !lens)) {
					free(vals);
					free(lens);
					continue;
				}
				for (i = 0; i < n; i++) {
					char *s = duckdb_enum_dictionary_value(lt, i);
					size_t len;

					if (!s) {
						ok = false;
						break;
					}
					len = strlen(s);
					vals[i] = malloc(len + 1);
					if (!vals[i]) {
						duckdb_free(s);
						ok = false;
						break;
					}
					memcpy(vals[i], s, len + 1);
					lens[i] = len;
					duckdb_free(s);
				}
				if (!ok) {
					for (j = 0; j < i; j++) {
						free(vals[j]);
					}
					free(vals);
					free(lens);
					continue;
				}
				a->cols[c].is_enum = true;
				a->cols[c].enum_internal = internal;
				a->cols[c].enum_vals = vals;
				a->cols[c].enum_lens = lens;
				a->cols[c].enum_size = n;
			}
		}
	}
	a->owner = S;
	a->ncols = S->col_count;
}

static bool pdo_duckdb_decimal_from_vector(duckdb_logical_type lt, void *data, idx_t row, duckdb_decimal *d)
{
	d->width = duckdb_decimal_width(lt);
	d->scale = duckdb_decimal_scale(lt);
	if (!pdo_duckdb_decimal_meta_ok(d->width, d->scale)) {
		return false;
	}

	switch (duckdb_decimal_internal_type(lt)) {
		case DUCKDB_TYPE_SMALLINT: {
			int64_t v = ((int16_t *)data)[row];
			d->value.lower = (uint64_t)v;
			d->value.upper = v < 0 ? -1 : 0;
			return true;
		}
		case DUCKDB_TYPE_INTEGER: {
			int64_t v = ((int32_t *)data)[row];
			d->value.lower = (uint64_t)v;
			d->value.upper = v < 0 ? -1 : 0;
			return true;
		}
		case DUCKDB_TYPE_BIGINT: {
			int64_t v = ((int64_t *)data)[row];
			d->value.lower = (uint64_t)v;
			d->value.upper = v < 0 ? -1 : 0;
			return true;
		}
		case DUCKDB_TYPE_HUGEINT:
			d->value = ((duckdb_hugeint *)data)[row];
			return true;
		default:
			/* Unknown internal width — only reachable from a corrupt storage file.
			 * Don't fall through to a 16-byte HUGEINT read against a possibly
			 * narrower vector. */
			return false;
	}
}

static duckdb_value pdo_duckdb_decimal_value(duckdb_logical_type lt, void *data, idx_t row)
{
	duckdb_decimal d;

	if (!pdo_duckdb_decimal_from_vector(lt, data, row, &d)) {
		return duckdb_create_null_value();
	}
	return duckdb_create_decimal(d);
}

/* Shared decimal-point placement (CR-009): renders already-formatted magnitude
 * digits with the type's width/scale, including DuckDB's leading-zero rule
 * (the integer-part '0' is printed only when width > scale). Logic moved
 * verbatim out of the monolithic renderer so the 128-bit and 64-bit
 * magnitude paths share it. */
static zend_string *pdo_duckdb_decimal_digits_to_string(const char *digits, size_t len, bool neg, uint8_t width, uint8_t scale)
{
	char out[43];
	size_t pos = 0, int_len;

	if (neg) {
		out[pos++] = '-';
	}

	if (scale == 0) {
		memcpy(out + pos, digits, len);
		return zend_string_init(out, pos + len, 0);
	}

	if (len <= scale) {
		size_t zeros = (size_t)scale - len;
		/* DuckDB writes the integer-part '0' only when the type has room for an
		 * integer digit: DECIMAL(4,2) renders 0.05, DECIMAL(2,2) renders .05. */
		if (width > scale) {
			out[pos++] = '0';
		}
		if (pos + 1 + zeros + len > sizeof(out)) {
			return zend_string_init("", 0, 0);
		}
		out[pos++] = '.';
		memset(out + pos, '0', zeros);
		pos += zeros;
		memcpy(out + pos, digits, len);
		return zend_string_init(out, pos + len, 0);
	}

	int_len = len - scale;
	memcpy(out + pos, digits, int_len);
	pos += int_len;
	out[pos++] = '.';
	memcpy(out + pos, digits + int_len, scale);
	return zend_string_init(out, pos + scale, 0);
}

static zend_string *pdo_duckdb_decimal_to_string(duckdb_decimal d)
{
	char digits[40];
	uint64_t upper = (uint64_t)d.value.upper;
	uint64_t lower = d.value.lower;
	bool neg = d.value.upper < 0;
	size_t len;

	ZEND_ASSERT(pdo_duckdb_decimal_meta_ok(d.width, d.scale));
	if (neg) {
		lower = ~lower + 1;
		upper = ~upper + (lower == 0 ? 1 : 0);
	}

	len = pdo_duckdb_format_u128(upper, lower, digits);
	return pdo_duckdb_decimal_digits_to_string(digits, len, neg, d.width, d.scale);
}

static size_t pdo_duckdb_append_fraction(char *buf, size_t len, int64_t fraction, int width)
{
	char frac[10];
	int keep = width;

	if (fraction == 0) {
		return len;
	}

	snprintf(frac, sizeof(frac), "%0*" PRId64, width, fraction);
	while (keep > 0 && frac[keep - 1] == '0') {
		keep--;
	}
	buf[len++] = '.';
	memcpy(buf + len, frac, (size_t)keep);
	return len + (size_t)keep;
}

static bool pdo_duckdb_format_date_part(duckdb_date date, char *buf, size_t *len)
{
	duckdb_date_struct ds;
	int n;

	if (!duckdb_is_finite_date(date)) {
		return false;
	}

	ds = duckdb_from_date(date);
	if (ds.year <= 0) {
		return false;
	}

	n = snprintf(buf, 32, "%04d-%02d-%02d", ds.year, ds.month, ds.day);
	if (n < 0 || n >= 32) {
		return false;
	}
	*len = (size_t)n;
	return true;
}

static size_t pdo_duckdb_format_time_part(duckdb_time_struct ts, char *buf)
{
	size_t len = (size_t)snprintf(buf, 32, "%02d:%02d:%02d", ts.hour, ts.min, ts.sec);
	return pdo_duckdb_append_fraction(buf, len, ts.micros, 6);
}

static size_t pdo_duckdb_format_tz_offset(int32_t offset, char *buf)
{
	int64_t off = offset;
	char sign = '+';
	int64_t hours, minutes, seconds;
	int n;

	if (off < 0) {
		sign = '-';
		off = -off;
	}

	hours = off / 3600;
	minutes = (off % 3600) / 60;
	seconds = off % 60;
	if (seconds) {
		n = snprintf(buf, 16, "%c%02" PRId64 ":%02" PRId64 ":%02" PRId64, sign, hours, minutes, seconds);
	} else if (minutes) {
		n = snprintf(buf, 16, "%c%02" PRId64 ":%02" PRId64, sign, hours, minutes);
	} else {
		n = snprintf(buf, 16, "%c%02" PRId64, sign, hours);
	}
	return (n > 0 && n < 16) ? (size_t)n : 0;
}

static uint64_t pdo_duckdb_abs_i64(int64_t v)
{
	return v < 0 ? ((uint64_t) -(v + 1)) + 1 : (uint64_t)v;
}

/* 64-bit decimal magnitude path (CR-009): SMALLINT/INTEGER/BIGINT-internal
 * decimals always fit an int64, so they render through format_u64 and never
 * touch 128-bit division (nor the no-INT128 fallback bit loop). Output is
 * identical: the magnitude digits feed the same shared placement above. */
static zend_string *pdo_duckdb_decimal_to_string_i64(int64_t value, uint8_t width, uint8_t scale)
{
	char digits[20];
	bool neg = value < 0;
	size_t len = pdo_duckdb_format_u64(neg ? pdo_duckdb_abs_i64(value) : (uint64_t)value, digits);

	return pdo_duckdb_decimal_digits_to_string(digits, len, neg, width, scale);
}

/* Top-level DECIMAL cell from the CR-009 column cache: no per-cell
 * duckdb_decimal_width/scale/internal_type calls, 64-bit magnitudes without
 * 128-bit division. HUGEINT-internal magnitudes that truly exceed 64 bits
 * take the 128-bit formatter. Meta was validated at cache time. */
static bool pdo_duckdb_cached_decimal_to_zval(const pdo_duckdb_col_aux *aux, void *data, idx_t row, zval *result)
{
	switch (aux->dec_internal) {
		case DUCKDB_TYPE_SMALLINT:
			ZVAL_STR(result, pdo_duckdb_decimal_to_string_i64(
				((int16_t *)data)[row], aux->dec_width, aux->dec_scale));
			return true;
		case DUCKDB_TYPE_INTEGER:
			ZVAL_STR(result, pdo_duckdb_decimal_to_string_i64(
				((int32_t *)data)[row], aux->dec_width, aux->dec_scale));
			return true;
		case DUCKDB_TYPE_BIGINT:
			ZVAL_STR(result, pdo_duckdb_decimal_to_string_i64(
				((int64_t *)data)[row], aux->dec_width, aux->dec_scale));
			return true;
		case DUCKDB_TYPE_HUGEINT: {
			duckdb_hugeint h = ((duckdb_hugeint *)data)[row];
			uint64_t upper = (uint64_t)h.upper;
			uint64_t lower = h.lower;
			bool neg = h.upper < 0;

			if (neg) {
				lower = ~lower + 1;
				upper = ~upper + (lower == 0 ? 1 : 0);
			}
			if (upper == 0) {
				char digits[20];
				size_t len = pdo_duckdb_format_u64(lower, digits);

				ZVAL_STR(result, pdo_duckdb_decimal_digits_to_string(
					digits, len, neg, aux->dec_width, aux->dec_scale));
			} else {
				char digits[40];
				size_t len = pdo_duckdb_format_u128(upper, lower, digits);

				ZVAL_STR(result, pdo_duckdb_decimal_digits_to_string(
					digits, len, neg, aux->dec_width, aux->dec_scale));
			}
			return true;
		}
		default:
			return false;
	}
}

/* Top-level ENUM cell from the CR-009 interned dictionary: the index width
 * was cached, the label is a bounds-checked copy. Out-of-range indexes (only
 * from a corrupt file) return false so the generic path renders SQL NULL as
 * before. */
static bool pdo_duckdb_cached_enum_to_zval(const pdo_duckdb_col_aux *aux, void *data, idx_t row, zval *result)
{
	uint64_t idx;

	switch (aux->enum_internal) {
		case DUCKDB_TYPE_USMALLINT: idx = ((uint16_t *)data)[row]; break;
		case DUCKDB_TYPE_UINTEGER:  idx = ((uint32_t *)data)[row]; break;
		case DUCKDB_TYPE_UTINYINT:  idx = ((uint8_t *)data)[row]; break;
		default: return false;
	}
	if (idx >= aux->enum_size) {
		return false;
	}
	ZVAL_STRINGL(result, aux->enum_vals[idx], aux->enum_lens[idx]);
	return true;
}

static size_t pdo_duckdb_format_interval_time_part(int64_t micros, char *buf)
{
	uint64_t abs_micros = pdo_duckdb_abs_i64(micros);
	uint64_t hours = abs_micros / 3600000000ULL;
	uint64_t minutes, seconds, frac;
	size_t pos = 0;
	int n;

	abs_micros %= 3600000000ULL;
	minutes = abs_micros / 60000000ULL;
	abs_micros %= 60000000ULL;
	seconds = abs_micros / 1000000ULL;
	frac = abs_micros % 1000000ULL;

	if (micros < 0) {
		buf[pos++] = '-';
	}
	n = snprintf(buf + pos, 64 - pos, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes, seconds);
	if (n < 0 || n >= (int)(64 - pos)) {
		return 0;
	}
	pos += (size_t)n;
	return pdo_duckdb_append_fraction(buf, pos, (int64_t)frac, 6);
}

static bool pdo_duckdb_interval_to_string(duckdb_interval iv, zval *result)
{
	char buf[96];
	size_t len = 0;
	int n;

	if (iv.months != 0) {
		return false;
	}
	if (iv.days != 0) {
		n = snprintf(buf, sizeof(buf), "%d day%s", iv.days,
			(iv.days == 1 || iv.days == -1) ? "" : "s");
		if (n < 0 || n >= (int)sizeof(buf)) {
			return false;
		}
		len = (size_t)n;
		if (iv.micros != 0) {
			size_t tlen;

			buf[len++] = ' ';
			tlen = pdo_duckdb_format_interval_time_part(iv.micros, buf + len);
			if (tlen == 0 || len + tlen >= sizeof(buf)) {
				return false;
			}
			len += tlen;
		}
	} else {
		len = pdo_duckdb_format_interval_time_part(iv.micros, buf);
		if (len == 0 || len >= sizeof(buf)) {
			return false;
		}
	}

	ZVAL_STRINGL(result, buf, len);
	return true;
}

static bool pdo_duckdb_bit_to_string(duckdb_string_t s, zval *result)
{
	const uint8_t *raw = (const uint8_t *)duckdb_string_t_data(&s);
	idx_t size = duckdb_string_t_length(s);
	uint8_t padding;
	idx_t payload_bits, bit_count, i;
	zend_string *out;

	if (size == 0) {
		return false;
	}
	padding = raw[0];
	if (padding > 7 || size - 1 > (idx_t)SIZE_MAX / 8) {
		return false;
	}
	payload_bits = (size - 1) * 8;
	if (padding > payload_bits) {
		return false;
	}
	bit_count = payload_bits - padding;

	out = zend_string_alloc((size_t)bit_count, 0);
	for (i = 0; i < bit_count; i++) {
		idx_t payload_bit = padding + i;
		uint8_t byte = raw[1 + payload_bit / 8];
		uint8_t mask = (uint8_t)(1u << (7 - (payload_bit % 8)));
		ZSTR_VAL(out)[i] = (byte & mask) ? '1' : '0';
	}
	ZSTR_VAL(out)[bit_count] = '\0';
	ZVAL_STR(result, out);
	return true;
}

static bool pdo_duckdb_fast_col_to_string(duckdb_type tid, duckdb_logical_type lt, void *data, idx_t row, zval *result)
{
	char buf[64];
	size_t len;

	switch (tid) {
		case DUCKDB_TYPE_UBIGINT: {
			char tmp[32];
			int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, ((uint64_t *)data)[row]);
			if (n < 0 || n >= (int)sizeof(tmp)) {
				return false;
			}
			ZVAL_STRINGL(result, tmp, (size_t)n);
			return true;
		}
		case DUCKDB_TYPE_HUGEINT:
			ZVAL_STR(result, pdo_duckdb_hugeint_to_string(((duckdb_hugeint *)data)[row]));
			return true;
		case DUCKDB_TYPE_UHUGEINT:
			ZVAL_STR(result, pdo_duckdb_uhugeint_to_string(((duckdb_uhugeint *)data)[row]));
			return true;
		case DUCKDB_TYPE_DECIMAL: {
			duckdb_decimal d;
			if (!pdo_duckdb_decimal_from_vector(lt, data, row, &d)) {
				return false;
			}
			ZVAL_STR(result, pdo_duckdb_decimal_to_string(d));
			return true;
		}
		case DUCKDB_TYPE_DATE:
			if (!pdo_duckdb_format_date_part(((duckdb_date *)data)[row], buf, &len)) {
				return false;
			}
			ZVAL_STRINGL(result, buf, len);
			return true;
		case DUCKDB_TYPE_TIME:
			len = pdo_duckdb_format_time_part(duckdb_from_time(((duckdb_time *)data)[row]), buf);
			ZVAL_STRINGL(result, buf, len);
			return true;
		case DUCKDB_TYPE_TIME_TZ: {
			duckdb_time_tz_struct ts = duckdb_from_time_tz(((duckdb_time_tz *)data)[row]);
			size_t olen;

			len = pdo_duckdb_format_time_part(ts.time, buf);
			olen = pdo_duckdb_format_tz_offset(ts.offset, buf + len);
			if (olen == 0 || len + olen >= sizeof(buf)) {
				return false;
			}
			ZVAL_STRINGL(result, buf, len + olen);
			return true;
		}
		case DUCKDB_TYPE_TIME_NS: {
			int64_t nanos = ((int64_t *)data)[row];
			duckdb_time_struct ts;
			if (nanos < 0 || nanos >= 86400LL * 1000000000LL) {
				return false;
			}
			ts.hour = (int8_t)(nanos / (3600LL * 1000000000LL));
			nanos %= 3600LL * 1000000000LL;
			ts.min = (int8_t)(nanos / (60LL * 1000000000LL));
			nanos %= 60LL * 1000000000LL;
			ts.sec = (int8_t)(nanos / 1000000000LL);
			len = (size_t)snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ts.hour, ts.min, ts.sec);
			len = pdo_duckdb_append_fraction(buf, len, nanos % 1000000000LL, 9);
			ZVAL_STRINGL(result, buf, len);
			return true;
		}
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIMESTAMP_TZ: {
			duckdb_timestamp ts = ((duckdb_timestamp *)data)[row];
			duckdb_timestamp_struct parts;
			size_t dlen;

			if (!duckdb_is_finite_timestamp(ts)) {
				return false;
			}
			parts = duckdb_from_timestamp(ts);
			if (!pdo_duckdb_format_date_part(duckdb_to_date(parts.date), buf, &dlen)) {
				return false;
			}
			buf[dlen++] = ' ';
			len = pdo_duckdb_format_time_part(parts.time, buf + dlen);
			if (tid == DUCKDB_TYPE_TIMESTAMP_TZ) {
				memcpy(buf + dlen + len, "+00", 3);
				len += 3;
			}
			ZVAL_STRINGL(result, buf, dlen + len);
			return true;
		}
		case DUCKDB_TYPE_INTERVAL:
			return pdo_duckdb_interval_to_string(((duckdb_interval *)data)[row], result);
		case DUCKDB_TYPE_ENUM: {
			uint64_t idx;
			char *s;
			switch (duckdb_enum_internal_type(lt)) {
				case DUCKDB_TYPE_USMALLINT: idx = ((uint16_t *)data)[row]; break;
				case DUCKDB_TYPE_UINTEGER:  idx = ((uint32_t *)data)[row]; break;
				case DUCKDB_TYPE_UTINYINT:
				default:                    idx = ((uint8_t *)data)[row];  break;
			}
			if (idx >= duckdb_enum_dictionary_size(lt)) {
				return false;
			}
			s = duckdb_enum_dictionary_value(lt, idx);
			if (!s) {
				return false;
			}
			ZVAL_STRING(result, s);
			duckdb_free(s);
			return true;
		}
		case DUCKDB_TYPE_UUID: {
			duckdb_uhugeint u = ((duckdb_uhugeint *)data)[row];
			uint64_t tail;

			u.upper ^= ((uint64_t)1 << 63);
			tail = u.lower & UINT64_C(0x0000ffffffffffff);
			snprintf(buf, sizeof(buf),
				"%08" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%012" PRIx64,
				u.upper >> 32,
				(u.upper >> 16) & 0xffff,
				u.upper & 0xffff,
				u.lower >> 48,
				tail);
			ZVAL_STRINGL(result, buf, 36);
			return true;
		}
		case DUCKDB_TYPE_BIT:
			return pdo_duckdb_bit_to_string(((duckdb_string_t *)data)[row], result);
		default:
			return false;
	}
}

static void pdo_duckdb_smart_append_u64(smart_str *out, uint64_t value)
{
	char digits[40];
	size_t len = pdo_duckdb_format_u128(0, value, digits);
	smart_str_appendl(out, digits, len);
}

static void pdo_duckdb_smart_append_i64(smart_str *out, int64_t value)
{
	if (value < 0) {
		smart_str_appendc(out, '-');
		pdo_duckdb_smart_append_u64(out, pdo_duckdb_abs_i64(value));
	} else {
		pdo_duckdb_smart_append_u64(out, (uint64_t)value);
	}
}

/* Nested VARCHAR quoting (CR-002): mirrors DuckDB's
 * VectorCastHelpers::Calculate/WriteEscapedString<false> (the list/struct/map
 * scalar positions in function/cast/vector_cast_helpers.hpp) and
 * NestedToVarcharCast::LOOKUP_TABLE (nested_to_varchar_cast.cpp). A value is
 * single-quoted (backslash-escaping only '\'' and '\\') when empty, padded
 * with recognized whitespace, case-insensitively "null", or containing one
 * of "'(),:=[]{}; otherwise it is emitted bare. UNION members render raw in
 * the engine, so the recursion below passes escape=false for those. */
static const bool pdo_duckdb_varchar_quote_table[256] = {
	['"'] = true, ['\''] = true, ['('] = true, [')'] = true, [','] = true,
	[':'] = true, ['='] = true, ['['] = true, [']'] = true, ['{'] = true,
	['}'] = true,
};

static bool pdo_duckdb_nested_varchar_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool pdo_duckdb_nested_varchar_needs_quotes(const char *p, size_t len)
{
	size_t i;

	if (len == 0) {
		return true;
	}
	if (pdo_duckdb_nested_varchar_is_space(p[0])) {
		return true;
	}
	if (len >= 2 && pdo_duckdb_nested_varchar_is_space(p[len - 1])) {
		return true;
	}
	if (len == 4
			&& (p[0] == 'n' || p[0] == 'N') && (p[1] == 'u' || p[1] == 'U')
			&& (p[2] == 'l' || p[2] == 'L') && (p[3] == 'l' || p[3] == 'L')) {
		return true;
	}
	for (i = 0; i < len; i++) {
		if (pdo_duckdb_varchar_quote_table[(unsigned char)p[i]]) {
			return true;
		}
	}
	return false;
}

/* Append a nested VARCHAR leaf. needs_quotes is decided on the full value
 * (like the engine); emission stops at the first NUL so the result matches
 * the legacy duckdb_get_varchar C-string truncation byte-for-byte (notably,
 * no closing quote is emitted after a truncation). */
static void pdo_duckdb_smart_append_escaped_varchar(smart_str *out, const char *p, size_t len)
{
	size_t i;

	if (len == 0) {
		smart_str_appends(out, "''");
		return;
	}
	if (!pdo_duckdb_nested_varchar_needs_quotes(p, len)) {
		const char *nul = memchr(p, '\0', len);

		smart_str_appendl(out, p, nul ? (size_t)(nul - p) : len);
		return;
	}
	smart_str_appendc(out, '\'');
	for (i = 0; i < len && p[i] != '\0'; i++) {
		if (p[i] == '\'' || p[i] == '\\') {
			smart_str_appendc(out, '\\');
		}
		smart_str_appendc(out, p[i]);
	}
	if (i == len) {
		smart_str_appendc(out, '\'');
	}
}

/* Nested FLOAT/DOUBLE leaf (CR-002): the engine renders floats with
 * duckdb_fmt shortest-round-trip ("1", "2.5", "1e+100", "inf"/"-inf"/"nan";
 * see the engine's nan_cast/infinity tests), which has no cheap exact
 * reimplementation in C. Render through the engine's scalar primitive instead
 * of the recursive cell_to_value_typed reconstruct: byte-identical by
 * construction (both go through StringCast::Operation(float/double)), minus
 * the recursion and logical-type handling. */
static bool pdo_duckdb_nested_float_to_smart_str(duckdb_type tid, void *data, idx_t row, smart_str *out)
{
	duckdb_value v = tid == DUCKDB_TYPE_FLOAT
		? duckdb_create_float(((float *)data)[row])
		: duckdb_create_double(((double *)data)[row]);
	char *s = duckdb_get_varchar(v);

	if (!s) {
		duckdb_destroy_value(&v);
		return false;
	}
	smart_str_appends(out, s);
	duckdb_free(s);
	duckdb_destroy_value(&v);
	return true;
}

static bool pdo_duckdb_smart_append_fast_scalar(duckdb_type tid, duckdb_logical_type lt, void *data, idx_t row, smart_str *out, bool escape_varchar)
{
	zval tmp;

	switch (tid) {
		case DUCKDB_TYPE_SQLNULL:
			smart_str_appends(out, "NULL");
			return true;
		case DUCKDB_TYPE_BOOLEAN:
			smart_str_appends(out, ((bool *)data)[row] ? "true" : "false");
			return true;
		case DUCKDB_TYPE_TINYINT:
			pdo_duckdb_smart_append_i64(out, ((int8_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_SMALLINT:
			pdo_duckdb_smart_append_i64(out, ((int16_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_INTEGER:
			pdo_duckdb_smart_append_i64(out, ((int32_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_BIGINT:
			pdo_duckdb_smart_append_i64(out, ((int64_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_UTINYINT:
			pdo_duckdb_smart_append_u64(out, ((uint8_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_USMALLINT:
			pdo_duckdb_smart_append_u64(out, ((uint16_t *)data)[row]);
			return true;
		case DUCKDB_TYPE_UINTEGER:
			pdo_duckdb_smart_append_u64(out, ((uint32_t *)data)[row]);
			return true;
		default:
			break;
	}

	switch (tid) {
		case DUCKDB_TYPE_UBIGINT:
		case DUCKDB_TYPE_HUGEINT:
		case DUCKDB_TYPE_UHUGEINT:
		case DUCKDB_TYPE_DECIMAL:
		case DUCKDB_TYPE_DATE:
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIME:
		case DUCKDB_TYPE_UUID:
			ZVAL_UNDEF(&tmp);
			if (!pdo_duckdb_fast_col_to_string(tid, lt, data, row, &tmp)) {
				return false;
			}
			if (Z_TYPE(tmp) != IS_STRING) {
				zval_ptr_dtor(&tmp);
				return false;
			}
			smart_str_appendl(out, Z_STRVAL(tmp), Z_STRLEN(tmp));
			zval_ptr_dtor(&tmp);
			return true;
		case DUCKDB_TYPE_VARCHAR: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			const char *p = duckdb_string_t_data(&s);
			size_t len = (size_t)duckdb_string_t_length(s);

			if (escape_varchar) {
				pdo_duckdb_smart_append_escaped_varchar(out, p, len);
			} else {
				const char *nul = memchr(p, '\0', len);

				smart_str_appendl(out, p, nul ? (size_t)(nul - p) : len);
			}
			return true;
		}
		case DUCKDB_TYPE_FLOAT:
		case DUCKDB_TYPE_DOUBLE:
			return pdo_duckdb_nested_float_to_smart_str(tid, data, row, out);
		default:
			return false;
	}
}

static bool pdo_duckdb_struct_name_is_fast_safe(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!name) {
		return false;
	}
	while (*p) {
		if (!((*p >= 'a' && *p <= 'z') ||
				(*p >= 'A' && *p <= 'Z') ||
				(*p >= '0' && *p <= '9') ||
				*p == '_')) {
			return false;
		}
		p++;
	}
	return true;
}

static bool pdo_duckdb_decimal_type_fast_safe(duckdb_logical_type lt)
{
	if (!pdo_duckdb_decimal_meta_ok(duckdb_decimal_width(lt), duckdb_decimal_scale(lt))) {
		return false;
	}
	switch (duckdb_decimal_internal_type(lt)) {
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_HUGEINT:
			return true;
		default:
			return false;
	}
}

static void pdo_duckdb_nested_render_type_destroy(pdo_duckdb_nested_render_type *type)
{
	idx_t i;

	if (!type) {
		return;
	}
	for (i = 0; i < type->child_count; i++) {
		pdo_duckdb_nested_render_type_destroy(type->children[i]);
		if (type->child_names && type->child_names[i]) {
			duckdb_free(type->child_names[i]);
		}
	}
	if (type->children) {
		efree(type->children);
	}
	if (type->child_names) {
		efree(type->child_names);
	}
	if (type->owns_logical_type && type->logical_type) {
		duckdb_destroy_logical_type(&type->logical_type);
	}
	efree(type);
}

static pdo_duckdb_nested_render_type *pdo_duckdb_nested_render_type_build(
		duckdb_type tid, duckdb_logical_type lt, bool owns_lt)
{
	pdo_duckdb_nested_render_type *type = ecalloc(1, sizeof(*type));
	idx_t i;

	type->type = tid;
	type->logical_type = lt;
	type->owns_logical_type = owns_lt;

	switch (tid) {
		case DUCKDB_TYPE_LIST: {
			duckdb_logical_type child = duckdb_list_type_child_type(lt);
			type->child_count = 1;
			type->children = ecalloc(1, sizeof(*type->children));
			type->children[0] = pdo_duckdb_nested_render_type_build(
				duckdb_get_type_id(child), child, true);
			if (!type->children[0]) {
				pdo_duckdb_nested_render_type_destroy(type);
				return NULL;
			}
			return type;
		}
		case DUCKDB_TYPE_ARRAY: {
			duckdb_logical_type child = duckdb_array_type_child_type(lt);
			type->child_count = 1;
			type->children = ecalloc(1, sizeof(*type->children));
			type->children[0] = pdo_duckdb_nested_render_type_build(
				duckdb_get_type_id(child), child, true);
			if (!type->children[0]) {
				pdo_duckdb_nested_render_type_destroy(type);
				return NULL;
			}
			return type;
		}
		case DUCKDB_TYPE_STRUCT:
			type->child_count = duckdb_struct_type_child_count(lt);
			type->children = ecalloc(type->child_count, sizeof(*type->children));
			type->child_names = ecalloc(type->child_count, sizeof(*type->child_names));
			for (i = 0; i < type->child_count; i++) {
				duckdb_logical_type child = duckdb_struct_type_child_type(lt, i);
				char *name = duckdb_struct_type_child_name(lt, i);
				if (!pdo_duckdb_struct_name_is_fast_safe(name)) {
					if (name) {
						duckdb_free(name);
					}
					duckdb_destroy_logical_type(&child);
					pdo_duckdb_nested_render_type_destroy(type);
					return NULL;
				}
				type->child_names[i] = name;
				type->children[i] = pdo_duckdb_nested_render_type_build(
					duckdb_get_type_id(child), child, true);
				if (!type->children[i]) {
					pdo_duckdb_nested_render_type_destroy(type);
					return NULL;
				}
			}
			return type;
		case DUCKDB_TYPE_MAP: {
			duckdb_logical_type key = duckdb_map_type_key_type(lt);
			duckdb_logical_type value = duckdb_map_type_value_type(lt);
			type->child_count = 2;
			type->children = ecalloc(2, sizeof(*type->children));
			type->children[0] = pdo_duckdb_nested_render_type_build(
				duckdb_get_type_id(key), key, true);
			if (!type->children[0]) {
				duckdb_destroy_logical_type(&value);
				pdo_duckdb_nested_render_type_destroy(type);
				return NULL;
			}
			type->children[1] = pdo_duckdb_nested_render_type_build(
				duckdb_get_type_id(value), value, true);
			if (!type->children[1]) {
				pdo_duckdb_nested_render_type_destroy(type);
				return NULL;
			}
			return type;
		}
		case DUCKDB_TYPE_UNION:
			type->child_count = duckdb_union_type_member_count(lt);
			type->children = ecalloc(type->child_count, sizeof(*type->children));
			for (i = 0; i < type->child_count; i++) {
				duckdb_logical_type member = duckdb_union_type_member_type(lt, i);
				type->children[i] = pdo_duckdb_nested_render_type_build(
					duckdb_get_type_id(member), member, true);
				if (!type->children[i]) {
					pdo_duckdb_nested_render_type_destroy(type);
					return NULL;
				}
			}
			return type;
		case DUCKDB_TYPE_SQLNULL:
		case DUCKDB_TYPE_BOOLEAN:
		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER:
		case DUCKDB_TYPE_UBIGINT:
		case DUCKDB_TYPE_HUGEINT:
		case DUCKDB_TYPE_UHUGEINT:
		case DUCKDB_TYPE_DATE:
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIME:
		case DUCKDB_TYPE_FLOAT:
		case DUCKDB_TYPE_DOUBLE:
		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_UUID:
			return type;
		case DUCKDB_TYPE_DECIMAL:
			if (pdo_duckdb_decimal_type_fast_safe(lt)) {
				return type;
			}
			break;
		default:
			break;
	}
	pdo_duckdb_nested_render_type_destroy(type);
	return NULL;
}

static bool pdo_duckdb_fast_nested_cell_to_smart_str(duckdb_vector vec, idx_t row,
		const pdo_duckdb_nested_render_type *type, smart_str *out, bool escape_varchar)
{
	uint64_t *validity = duckdb_vector_get_validity(vec);
	void *data;

	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		smart_str_appends(out, "NULL");
		return true;
	}

	data = duckdb_vector_get_data(vec);

	switch (type->type) {
		case DUCKDB_TYPE_LIST: {
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			duckdb_vector child = duckdb_list_vector_get_child(vec);
			idx_t child_size = duckdb_list_vector_get_size(vec);
			idx_t i;

			if (e.offset > child_size || e.length > child_size - e.offset) {
				return false;
			}

			smart_str_appendc(out, '[');
			for (i = 0; i < e.length; i++) {
				if (i) {
					smart_str_appends(out, ", ");
				}
				if (!pdo_duckdb_fast_nested_cell_to_smart_str(
						child, e.offset + i, type->children[0], out, true)) {
					return false;
				}
			}
			smart_str_appendc(out, ']');
			return true;
		}
		case DUCKDB_TYPE_ARRAY: {
			/* Unlike LIST, the engine's ArrayToVarcharCast joins pre-cast
			 * children with a raw memcpy (no quoting), so members render raw. */
			idx_t n = duckdb_array_type_array_size(type->logical_type);
			duckdb_vector child = duckdb_array_vector_get_child(vec);
			idx_t base, i;

			if (n && row > (idx_t)-1 / n) {
				return false;
			}

			base = row * n;
			smart_str_appendc(out, '[');
			for (i = 0; i < n; i++) {
				if (i) {
					smart_str_appends(out, ", ");
				}
				if (!pdo_duckdb_fast_nested_cell_to_smart_str(
						child, base + i, type->children[0], out, false)) {
					return false;
				}
			}
			smart_str_appendc(out, ']');
			return true;
		}
		case DUCKDB_TYPE_STRUCT: {
			idx_t i;

			smart_str_appendc(out, '{');
			for (i = 0; i < type->child_count; i++) {
				duckdb_vector child = duckdb_struct_vector_get_child(vec, i);

				if (i) {
					smart_str_appends(out, ", ");
				}
				smart_str_appendc(out, '\'');
				smart_str_appends(out, type->child_names[i]);
				smart_str_appends(out, "': ");
				if (!pdo_duckdb_fast_nested_cell_to_smart_str(
						child, row, type->children[i], out, true)) {
					return false;
				}
			}
			smart_str_appendc(out, '}');
			return true;
		}
		case DUCKDB_TYPE_MAP: {
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			idx_t entries_size = duckdb_list_vector_get_size(vec);
			duckdb_vector entries, kvec, vvec;
			idx_t i;

			if (e.offset > entries_size || e.length > entries_size - e.offset) {
				return false;
			}

			entries = duckdb_list_vector_get_child(vec);
			kvec = duckdb_struct_vector_get_child(entries, 0);
			vvec = duckdb_struct_vector_get_child(entries, 1);

			smart_str_appendc(out, '{');
			for (i = 0; i < e.length; i++) {
				if (i) {
					smart_str_appends(out, ", ");
				}
				if (!pdo_duckdb_fast_nested_cell_to_smart_str(
						kvec, e.offset + i, type->children[0], out, true)) {
					return false;
				}
				smart_str_appendc(out, '=');
				if (!pdo_duckdb_fast_nested_cell_to_smart_str(
						vvec, e.offset + i, type->children[1], out, true)) {
					return false;
				}
			}
			smart_str_appendc(out, '}');
			return true;
		}
		case DUCKDB_TYPE_UNION: {
			duckdb_vector tag_vec = duckdb_struct_vector_get_child(vec, 0);
			void *tag_data = duckdb_vector_get_data(tag_vec);
			idx_t tag = (idx_t)((uint8_t *)tag_data)[row];
			duckdb_vector member_vec;

			if (tag >= type->child_count) {
				return false;
			}

			member_vec = duckdb_struct_vector_get_child(vec, tag + 1);
			return pdo_duckdb_fast_nested_cell_to_smart_str(
				member_vec, row, type->children[tag], out, false);
		}
		default:
			return pdo_duckdb_smart_append_fast_scalar(
				type->type, type->logical_type, data, row, out, escape_varchar);
	}
}

static bool pdo_duckdb_fast_nested_col_to_string(duckdb_vector vec, idx_t row,
		const pdo_duckdb_nested_render_type *type, zval *result)
{
	smart_str out = {0};

	switch (type->type) {
		case DUCKDB_TYPE_LIST:
		case DUCKDB_TYPE_ARRAY:
		case DUCKDB_TYPE_STRUCT:
		case DUCKDB_TYPE_MAP:
		case DUCKDB_TYPE_UNION:
			break;
		default:
			return false;
	}

	if (!pdo_duckdb_fast_nested_cell_to_smart_str(vec, row, type, &out, true)) {
		smart_str_free(&out);
		return false;
	}

	ZVAL_STR(result, smart_str_extract(&out));
	return true;
}

/* Nested GEOMETRY substitution. The DUCKDB_TYPE_GEOMETRY case below encodes an
 * element as a VARCHAR hex value (the C API has no geometry value
 * constructor), but duckdb_create_*_value rejects values whose type differs
 * from the declared child type, so a container declared with a GEOMETRY child
 * would come back NULL. Rebuild a container logical type with VARCHAR in
 * place of GEOMETRY anywhere in its tree; element recursion still runs
 * against the original GEOMETRY logical types (which hit the hex encoder),
 * while the declared container type matches the produced VARCHAR values.
 * Borrows lt; on true *out owns a fresh logical type, otherwise *out is
 * untouched. Type constructors borrow their inputs, so rebuilt children are
 * destroyed after use like every other owned logical type here. */
static bool pdo_duckdb_substitute_geometry(duckdb_logical_type lt, duckdb_logical_type *out)
{
	duckdb_type tid = duckdb_get_type_id(lt);
	idx_t i;

	if (tid == DUCKDB_TYPE_GEOMETRY) {
		*out = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
		return true;
	}
	if (tid == DUCKDB_TYPE_LIST) {
		duckdb_logical_type child = duckdb_list_type_child_type(lt);
		duckdb_logical_type sub;
		bool changed = pdo_duckdb_substitute_geometry(child, &sub);
		if (changed) {
			*out = duckdb_create_list_type(sub);
			duckdb_destroy_logical_type(&sub);
		}
		duckdb_destroy_logical_type(&child);
		return changed;
	}
	if (tid == DUCKDB_TYPE_ARRAY) {
		duckdb_logical_type child = duckdb_array_type_child_type(lt);
		duckdb_logical_type sub;
		bool changed = pdo_duckdb_substitute_geometry(child, &sub);
		if (changed) {
			*out = duckdb_create_array_type(sub, duckdb_array_type_array_size(lt));
			duckdb_destroy_logical_type(&sub);
		}
		duckdb_destroy_logical_type(&child);
		return changed;
	}
	if (tid == DUCKDB_TYPE_STRUCT) {
		idx_t n = duckdb_struct_type_child_count(lt);
		duckdb_logical_type *members = n ? emalloc(sizeof(*members) * n) : NULL;
		const char **names = n ? emalloc(sizeof(*names) * n) : NULL;
		bool changed = false;
		for (i = 0; i < n; i++) {
			duckdb_logical_type child = duckdb_struct_type_child_type(lt, i);
			duckdb_logical_type sub;
			names[i] = duckdb_struct_type_child_name(lt, i);
			if (pdo_duckdb_substitute_geometry(child, &sub)) {
				members[i] = sub;
				duckdb_destroy_logical_type(&child);
				changed = true;
			} else {
				members[i] = child;
			}
		}
		if (changed) {
			*out = duckdb_create_struct_type(members, names, n);
		}
		for (i = 0; i < n; i++) {
			duckdb_destroy_logical_type(&members[i]);
			duckdb_free((void *)names[i]);
		}
		if (members) {
			efree(members);
		}
		if (names) {
			efree(names);
		}
		return changed;
	}
	if (tid == DUCKDB_TYPE_MAP) {
		duckdb_logical_type key = duckdb_map_type_key_type(lt);
		duckdb_logical_type value = duckdb_map_type_value_type(lt);
		duckdb_logical_type skey, svalue;
		bool changed = false;
		if (pdo_duckdb_substitute_geometry(key, &skey)) {
			changed = true;
		} else {
			skey = key;
		}
		if (pdo_duckdb_substitute_geometry(value, &svalue)) {
			changed = true;
		} else {
			svalue = value;
		}
		if (changed) {
			*out = duckdb_create_map_type(skey, svalue);
		}
		/* skey/svalue alias key/value when unchanged; destroy exactly once. */
		if (skey != key) {
			duckdb_destroy_logical_type(&skey);
		}
		if (svalue != value) {
			duckdb_destroy_logical_type(&svalue);
		}
		duckdb_destroy_logical_type(&key);
		duckdb_destroy_logical_type(&value);
		return changed;
	}
	if (tid == DUCKDB_TYPE_UNION) {
		idx_t n = duckdb_union_type_member_count(lt);
		duckdb_logical_type *members = n ? emalloc(sizeof(*members) * n) : NULL;
		const char **names = n ? emalloc(sizeof(*names) * n) : NULL;
		bool changed = false;
		for (i = 0; i < n; i++) {
			duckdb_logical_type member = duckdb_union_type_member_type(lt, i);
			duckdb_logical_type sub;
			names[i] = duckdb_union_type_member_name(lt, i);
			if (pdo_duckdb_substitute_geometry(member, &sub)) {
				members[i] = sub;
				duckdb_destroy_logical_type(&member);
				changed = true;
			} else {
				members[i] = member;
			}
		}
		if (changed) {
			*out = duckdb_create_union_type(members, names, n);
		}
		for (i = 0; i < n; i++) {
			duckdb_destroy_logical_type(&members[i]);
			duckdb_free((void *)names[i]);
		}
		if (members) {
			efree(members);
		}
		if (names) {
			efree(names);
		}
		return changed;
	}
	return false;
}

static duckdb_value pdo_duckdb_cell_to_value_typed(duckdb_vector vec, idx_t row,
		duckdb_logical_type lt, bool destroy_lt, bool *unsupported_variant)
{
	duckdb_type tid = duckdb_get_type_id(lt);
	uint64_t *validity = duckdb_vector_get_validity(vec);
	void *data;
	duckdb_value ret;

	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		if (destroy_lt) {
			duckdb_destroy_logical_type(&lt);
		}
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
		/* Sub-/super-second precision variants store a single int64 (seconds /
		 * millis / nanos since the epoch, nanos since midnight). Without these the
		 * default branch would render them as a silent SQL NULL. */
		case DUCKDB_TYPE_TIMESTAMP_S: {
			duckdb_timestamp_s t; t.seconds = ((int64_t *)data)[row];
			ret = duckdb_create_timestamp_s(t); break;
		}
		case DUCKDB_TYPE_TIMESTAMP_MS: {
			duckdb_timestamp_ms t; t.millis = ((int64_t *)data)[row];
			ret = duckdb_create_timestamp_ms(t); break;
		}
		case DUCKDB_TYPE_TIMESTAMP_NS: {
			duckdb_timestamp_ns t; t.nanos = ((int64_t *)data)[row];
			ret = duckdb_create_timestamp_ns(t); break;
		}
		case DUCKDB_TYPE_TIME_NS: {
			duckdb_time_ns t; t.nanos = ((int64_t *)data)[row];
			ret = duckdb_create_time_ns(t); break;
		}
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
			/* The first byte is a pad-bit-count header; a zero-length BIT (only
			 * from a corrupt file) would make create_bit read data[0] out of
			 * bounds. Guard, like the VARINT len < 3 check. */
			if (b.size == 0) {
				ret = duckdb_create_null_value();
				break;
			}
			ret = duckdb_create_bit(b);
			break;
		}
		case DUCKDB_TYPE_BIGNUM: {	/* the VARINT SQL type */
			/* DuckDB stores VARINT as a blob: a 3-byte header whose top bit is the
			 * sign (1 = positive), then the big-endian magnitude. Negative values
			 * are stored bitwise-complemented so byte order matches numeric order,
			 * so flip the magnitude bytes back. duckdb_create_bignum wants the plain
			 * big-endian absolute value + is_negative. */
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			const uint8_t *raw = (const uint8_t *)duckdb_string_t_data(&s);
			idx_t len = duckdb_string_t_length(s);

			if (len < 3) {
				ret = duckdb_create_null_value();
			} else {
				bool is_negative = (raw[0] & 0x80) == 0;
				idx_t mlen = len - 3;
				uint8_t *mag = emalloc(mlen ? mlen : 1);
				duckdb_bignum bn;
				idx_t i;

				for (i = 0; i < mlen; i++) {
					mag[i] = is_negative ? (uint8_t)~raw[3 + i] : raw[3 + i];
				}
				if (mlen == 0) {	/* zero magnitude */
					mag[0] = 0;
					mlen = 1;
					is_negative = false;
				}
				bn.data = mag;
				bn.size = mlen;
				bn.is_negative = is_negative;
				ret = duckdb_create_bignum(bn);
				efree(mag);
			}
			break;
		}
		case DUCKDB_TYPE_GEOMETRY: {
			/* GEOMETRY is stored as a flat WKB blob (duckdb_string_t in the
			 * vector). The C API has no WKB->WKT renderer, so expose the bytes
			 * as an uppercase hex string: lossless and round-trippable via
			 * ST_GeomFromHEXWKB(). Use ST_AsText() in SQL for WKT. */
			static const char hexd[] = "0123456789ABCDEF";
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			const uint8_t *raw = (const uint8_t *)duckdb_string_t_data(&s);
			idx_t len = duckdb_string_t_length(s);
			char *hex;
			idx_t i;
			if (len > ((idx_t)SIZE_MAX - 1) / 2) {
				ret = duckdb_create_null_value();
				break;
			}
			hex = emalloc((size_t)len * 2 + 1);
			for (i = 0; i < len; i++) {
				hex[i * 2]     = hexd[raw[i] >> 4];
				hex[i * 2 + 1] = hexd[raw[i] & 0x0F];
			}
			ret = duckdb_create_varchar_length(hex, len * 2);
			efree(hex);
			break;
		}

		case DUCKDB_TYPE_LIST: {
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			duckdb_vector child = duckdb_list_vector_get_child(vec);
			idx_t child_size = duckdb_list_vector_get_size(vec);
		duckdb_logical_type ct = duckdb_list_type_child_type(lt);
		duckdb_value *vals;
			idx_t i;
			/* {offset,length} is an engine invariant for any chunk DuckDB
			 * produces; a corrupt storage file could violate it. Clamp before
			 * indexing the child vector (overflow-safe form). */
			if (e.offset > child_size || e.length > child_size - e.offset) {
				duckdb_destroy_logical_type(&ct);
				ret = duckdb_create_null_value();
				break;
			}
		/* always non-NULL: create_list_value() segfaults on a NULL values
		 * pointer, so an empty list must still pass a valid buffer. */
		duckdb_logical_type decl = ct;
		/* GEOMETRY elements are encoded as VARCHAR hex values, so declare
		 * the child with a substituted type (no-op otherwise). */
		bool geo_sub = pdo_duckdb_substitute_geometry(ct, &decl);
		vals = emalloc(sizeof(duckdb_value) * (e.length ? e.length : 1));
			for (i = 0; i < e.length; i++) {
				vals[i] = pdo_duckdb_cell_to_value_typed(
					child, e.offset + i, ct, false, unsupported_variant);
			}
		ret = duckdb_create_list_value(geo_sub ? decl : ct, vals, e.length);
		for (i = 0; i < e.length; i++) {
			duckdb_destroy_value(&vals[i]);
		}
		efree(vals);
		if (geo_sub) {
			duckdb_destroy_logical_type(&decl);
		}
		duckdb_destroy_logical_type(&ct);
			break;
		}
		case DUCKDB_TYPE_ARRAY: {
			idx_t n = duckdb_array_type_array_size(lt);
			duckdb_vector child = duckdb_array_vector_get_child(vec);
			duckdb_logical_type ct = duckdb_array_type_child_type(lt);
			duckdb_value *vals;
			idx_t base, i;

			if (n && row > (idx_t)-1 / n) {
				duckdb_destroy_logical_type(&ct);
				ret = duckdb_create_null_value();
				break;
			}

		vals = emalloc(sizeof(duckdb_value) * (n ? n : 1));
		base = row * n;
		for (i = 0; i < n; i++) {
			vals[i] = pdo_duckdb_cell_to_value_typed(
				child, base + i, ct, false, unsupported_variant);
		}
		{
			duckdb_logical_type decl = ct;
			bool geo_sub = pdo_duckdb_substitute_geometry(ct, &decl);
			ret = duckdb_create_array_value(geo_sub ? decl : ct, vals, n);
			if (geo_sub) {
				duckdb_destroy_logical_type(&decl);
			}
		}
		for (i = 0; i < n; i++) {
			duckdb_destroy_value(&vals[i]);
		}
		efree(vals);
		duckdb_destroy_logical_type(&ct);
			break;
		}
		case DUCKDB_TYPE_STRUCT: {
			idx_t n = duckdb_struct_type_child_count(lt);
			duckdb_value *vals = n ? emalloc(sizeof(duckdb_value) * n) : NULL;
			idx_t i;
			for (i = 0; i < n; i++) {
				duckdb_vector child = duckdb_struct_vector_get_child(vec, i);
				duckdb_logical_type ct = duckdb_struct_type_child_type(lt, i);
				vals[i] = pdo_duckdb_cell_to_value_typed(
					child, row, ct, false, unsupported_variant);
				duckdb_destroy_logical_type(&ct);
			}
		{
			duckdb_logical_type decl = lt;
			bool geo_sub = pdo_duckdb_substitute_geometry(lt, &decl);
			ret = duckdb_create_struct_value(geo_sub ? decl : lt, vals);
			if (geo_sub) {
				duckdb_destroy_logical_type(&decl);
			}
		}
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
			idx_t entries_size = duckdb_list_vector_get_size(vec);
			duckdb_vector entries, kvec, vvec;
			duckdb_value *keys, *vals;
			idx_t i;
			/* clamp a corrupt {offset,length} against the entries vector. */
			if (e.offset > entries_size || e.length > entries_size - e.offset) {
				ret = duckdb_create_null_value();
				break;
			}
			entries = duckdb_list_vector_get_child(vec);
			kvec = duckdb_struct_vector_get_child(entries, 0);
			vvec = duckdb_struct_vector_get_child(entries, 1);
			duckdb_logical_type kt = duckdb_map_type_key_type(lt);
			duckdb_logical_type vt = duckdb_map_type_value_type(lt);
		keys = emalloc(sizeof(duckdb_value) * (e.length ? e.length : 1));
		vals = emalloc(sizeof(duckdb_value) * (e.length ? e.length : 1));
			for (i = 0; i < e.length; i++) {
				keys[i] = pdo_duckdb_cell_to_value_typed(
					kvec, e.offset + i, kt, false, unsupported_variant);
				vals[i] = pdo_duckdb_cell_to_value_typed(
					vvec, e.offset + i, vt, false, unsupported_variant);
			}
		{
			duckdb_logical_type decl = lt;
			bool geo_sub = pdo_duckdb_substitute_geometry(lt, &decl);
			ret = duckdb_create_map_value(geo_sub ? decl : lt, keys, vals, e.length);
			if (geo_sub) {
				duckdb_destroy_logical_type(&decl);
			}
		}
			for (i = 0; i < e.length; i++) {
				duckdb_destroy_value(&keys[i]);
				duckdb_destroy_value(&vals[i]);
			}
			efree(keys);
			efree(vals);
			duckdb_destroy_logical_type(&kt);
			duckdb_destroy_logical_type(&vt);
			break;
		}

		case DUCKDB_TYPE_UNION: {
			/* UNION is physically a STRUCT whose child 0 is the UTINYINT tag and
			 * children 1..n are the members. Read the tag, then reconstruct the
			 * active member (child tag+1) and wrap it. */
			duckdb_vector tag_vec = duckdb_struct_vector_get_child(vec, 0);
			void *tag_data = duckdb_vector_get_data(tag_vec);
			idx_t tag = (idx_t)((uint8_t *)tag_data)[row];
			duckdb_vector member_vec;
			/* The tag byte is the one place a raw data byte becomes a structural
			 * index. duckdb_struct_vector_get_child does no bounds check, so a
			 * corrupt-file tag past the member count would index out of bounds.
			 * Validate before indexing (members are children 1..n). */
			if (tag >= duckdb_union_type_member_count(lt)) {
				ret = duckdb_create_null_value();
				break;
			}
			member_vec = duckdb_struct_vector_get_child(vec, tag + 1);
			duckdb_logical_type mt = duckdb_union_type_member_type(lt, tag);
			duckdb_value member = pdo_duckdb_cell_to_value_typed(
				member_vec, row, mt, false, unsupported_variant);
		{
			duckdb_logical_type decl = lt;
			bool geo_sub = pdo_duckdb_substitute_geometry(lt, &decl);
			ret = duckdb_create_union_value(geo_sub ? decl : lt, tag, member);
			if (geo_sub) {
				duckdb_destroy_logical_type(&decl);
			}
		}
			duckdb_destroy_value(&member);
			duckdb_destroy_logical_type(&mt);
			break;
		}

		case DUCKDB_TYPE_VARIANT:
			if (unsupported_variant) {
				*unsupported_variant = true;
			}
			ret = duckdb_create_null_value();
			break;

		default:
			/* Unknown/unsupported type: surface as SQL NULL rather than crash. */
			ret = duckdb_create_null_value();
			break;
	}

	if (destroy_lt) {
		duckdb_destroy_logical_type(&lt);
	}
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

	/* Unbuffered scans can open files during chunk pull; re-latch the sandbox if
	 * open_basedir was tightened after execute. Pulling a chunk is the only part
	 * of fetch that reaches DuckDB — rows served out of the chunk above touch no
	 * filesystem — so the check belongs here rather than on every row. */
	if (!pdo_duckdb_enforce_sandbox(S->H)) {
		pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_SANDBOX, "Unable to apply the open_basedir sandbox profile to DuckDB");
		pdo_duckdb_stmt_reset_result_full(stmt);
		S->done = true;
		return 0;
	}

	/* Advance to the next non-empty chunk. */
	for (;;) {
		if (S->chunk) {
			pdo_duckdb_chunk_cache_invalidate(S->chunk);
			duckdb_destroy_data_chunk(&S->chunk);
			S->chunk = NULL;
		}
		S->chunk = duckdb_fetch_chunk(S->result);
		if (!S->chunk) {
			const char *err = duckdb_result_error(&S->result);
			if (err && *err) {
				/* Release the native result (esp. unbuffered streams) so the
				 * connection is not pinned until a later re-execute/dtor. */
				pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_STREAMING, err);
				pdo_duckdb_stmt_reset_result_full(stmt);
				S->done = true;
				return 0;
			}
			S->done = true;
			return 0;
		}
		/* Load the per-chunk column cache (CR-001) for the new chunk. */
		pdo_duckdb_chunk_cache_load(S);
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

	if (!S->has_result || (idx_t)colno >= S->col_count) {
		return 0;
	}

	name = duckdb_column_name(&S->result, (idx_t)colno);
	stmt->columns[colno].name = zend_string_init(name ? name : "", name ? strlen(name) : 0, 0);
	stmt->columns[colno].maxlen = SIZE_MAX;
	stmt->columns[colno].precision = 0;

	/* PDO core overwrites getColumnMeta()'s "precision" with col->precision, so a
	 * DECIMAL's total-digit width has to be reported here (scale is added in
	 * get_column_meta, where it survives). */
	if (S->col_types[colno] == DUCKDB_TYPE_DECIMAL) {
		stmt->columns[colno].precision = duckdb_decimal_width(S->col_logical_types[colno]);
	}

	return 1;
}

/* Reconstruct the cell as a duckdb_value and render its canonical string into
 * `result`. Used for types without a native PHP mapping, and for integers that
 * overflow zend_long on 32-bit builds. */
static bool pdo_duckdb_col_to_string(duckdb_vector vec, idx_t row,
		duckdb_logical_type lt, zval *result)
{
	bool unsupported_variant = false;
	duckdb_value v = pdo_duckdb_cell_to_value_typed(
		vec, row, lt, false, &unsupported_variant);

	if (unsupported_variant) {
		duckdb_destroy_value(&v);
		ZVAL_NULL(result);
		return false;
	}
	/* duckdb_get_varchar() on a NULL value throws a C++ InternalException that
	 * aborts the process; guard with is_null (also covers any unhandled type
	 * cell_to_value falls back to a NULL value for). */
	if (!v || duckdb_is_null_value(v)) {
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
	return true;
}

static int pdo_duckdb_stmt_get_col(
		pdo_stmt_t *stmt, int colno, zval *result, enum pdo_param_type *type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	pdo_duckdb_chunk_cache *cc = &pdo_duckdb_tls_chunk_cache;
	duckdb_vector vec;
	duckdb_type tid;
	uint64_t *validity;
	void *data;
	idx_t row;

	if (!S->chunk || S->cur >= S->chunk_size) {
		return 0;
	}
	/* col_types/col_logical_types are sized by the cached result column count;
	 * the chunk's own count equals it for anything DuckDB produces, but bound
	 * the index by the cache too rather than trusting that invariant. */
	if ((idx_t)colno >= S->col_count) {
		return 0;
	}
	/* CR-001: index the per-chunk column cache loaded in fetch() instead of
	 * re-querying the vector/validity/data pointers per cell. On a miss
	 * (another statement pulled a chunk since) refresh it best-effort; only
	 * when the cache itself is unavailable fall back to direct reads. */
	if (cc->chunk != S->chunk) {
		pdo_duckdb_chunk_cache_load(S);
	}
	if (cc->chunk == S->chunk) {
		if ((idx_t)colno >= cc->ncols) {
			return 0;
		}
		vec = cc->vecs[colno];
		validity = cc->validities[colno];
		data = cc->datas[colno];
	} else {
		if ((idx_t)colno >= duckdb_data_chunk_get_column_count(S->chunk)) {
			return 0;
		}
		vec = duckdb_data_chunk_get_vector(S->chunk, (idx_t)colno);
		validity = duckdb_vector_get_validity(vec);
		data = duckdb_vector_get_data(vec);
	}

	row = S->cur;
	tid = S->col_types[colno];

	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		ZVAL_NULL(result);
		return 1;
	}

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
				(void)pdo_duckdb_col_to_string(vec, row, S->col_logical_types[colno], result);  /* preserve precision as string */
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
				(void)pdo_duckdb_col_to_string(vec, row, S->col_logical_types[colno], result);
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

		case DUCKDB_TYPE_VARIANT:
			ZVAL_NULL(result);
			pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_STREAMING,
				"DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL");
			pdo_handle_error(stmt->dbh, stmt);
			return 1;

		default: {
			pdo_duckdb_col_aux *aux = pdo_duckdb_col_aux_for(S, colno);

			/* Extended scalar and nested types are returned as their canonical
			 * DuckDB string form; common scalar formats use fast renderers first. */
			if (aux != NULL) {
				if (aux->is_decimal
						&& pdo_duckdb_cached_decimal_to_zval(aux, data, row, result)) {
					return 1;
				}
				if (aux->is_enum
						&& pdo_duckdb_cached_enum_to_zval(aux, data, row, result)) {
					return 1;
				}
			}
			if (S->col_nested_renderers[colno] &&
					pdo_duckdb_fast_nested_col_to_string(
						vec, row, S->col_nested_renderers[colno], result)) {
				return 1;
			}
			if (pdo_duckdb_fast_col_to_string(
					tid, S->col_logical_types[colno], data, row, result)) {
				return 1;
			}
			if (!pdo_duckdb_col_to_string(
					vec, row, S->col_logical_types[colno], result)) {
				pdo_duckdb_error_stmt_code(stmt, PDO_DUCKDB_ERRCODE_STREAMING,
					"DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL");
				pdo_handle_error(stmt->dbh, stmt);
			}
			return 1;
		}
	}
}

/* Canonical DuckDB type name for getColumnMeta()'s native_type. Distinct from
 * the coarse buckets get_col uses to pick a zval kind: here every type reports
 * its real name (a DECIMAL is "DECIMAL", not "DOUBLE") so callers can tell, e.g.,
 * a TIMESTAMP from a UUID. */
static const char *pdo_duckdb_type_name(duckdb_type t)
{
	switch (t) {
		case DUCKDB_TYPE_BOOLEAN:      return "BOOLEAN";
		case DUCKDB_TYPE_TINYINT:      return "TINYINT";
		case DUCKDB_TYPE_SMALLINT:     return "SMALLINT";
		case DUCKDB_TYPE_INTEGER:      return "INTEGER";
		case DUCKDB_TYPE_BIGINT:       return "BIGINT";
		case DUCKDB_TYPE_UTINYINT:     return "UTINYINT";
		case DUCKDB_TYPE_USMALLINT:    return "USMALLINT";
		case DUCKDB_TYPE_UINTEGER:     return "UINTEGER";
		case DUCKDB_TYPE_UBIGINT:      return "UBIGINT";
		case DUCKDB_TYPE_HUGEINT:      return "HUGEINT";
		case DUCKDB_TYPE_UHUGEINT:     return "UHUGEINT";
		case DUCKDB_TYPE_FLOAT:        return "FLOAT";
		case DUCKDB_TYPE_DOUBLE:       return "DOUBLE";
		case DUCKDB_TYPE_DECIMAL:      return "DECIMAL";
		case DUCKDB_TYPE_VARCHAR:      return "VARCHAR";
		case DUCKDB_TYPE_BLOB:         return "BLOB";
		case DUCKDB_TYPE_DATE:         return "DATE";
		case DUCKDB_TYPE_TIME:         return "TIME";
		case DUCKDB_TYPE_TIME_TZ:      return "TIME_TZ";
		case DUCKDB_TYPE_TIME_NS:      return "TIME_NS";
		case DUCKDB_TYPE_TIMESTAMP:    return "TIMESTAMP";
		case DUCKDB_TYPE_TIMESTAMP_S:  return "TIMESTAMP_S";
		case DUCKDB_TYPE_TIMESTAMP_MS: return "TIMESTAMP_MS";
		case DUCKDB_TYPE_TIMESTAMP_NS: return "TIMESTAMP_NS";
		case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMP_TZ";
		case DUCKDB_TYPE_INTERVAL:     return "INTERVAL";
		case DUCKDB_TYPE_UUID:         return "UUID";
		case DUCKDB_TYPE_ENUM:         return "ENUM";
		case DUCKDB_TYPE_LIST:         return "LIST";
		case DUCKDB_TYPE_STRUCT:       return "STRUCT";
		case DUCKDB_TYPE_MAP:          return "MAP";
		case DUCKDB_TYPE_ARRAY:        return "ARRAY";
		case DUCKDB_TYPE_UNION:        return "UNION";
		case DUCKDB_TYPE_BIT:          return "BIT";
		case DUCKDB_TYPE_BIGNUM:       return "VARINT";
		case DUCKDB_TYPE_VARIANT:      return "VARIANT";
		case DUCKDB_TYPE_GEOMETRY:     return "GEOMETRY";
		case DUCKDB_TYPE_SQLNULL:      return "NULL";
		default:                       return "UNKNOWN";
	}
}

static int pdo_duckdb_stmt_col_meta(pdo_stmt_t *stmt, zend_long colno, zval *return_value)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	duckdb_logical_type lt;
	duckdb_type tid;
	enum pdo_param_type pdo_type;

	if (!S->has_result || (idx_t)colno >= S->col_count) {
		return FAILURE;
	}

	lt = S->col_logical_types[colno];
	tid = S->col_types[colno];

	array_init(return_value);
	add_assoc_string(return_value, "native_type", (char *)pdo_duckdb_type_name(tid));

	switch (tid) {
		/* BOOLEAN is PHP int 0/1 in get_col — same pdo_type as integer widths. */
		case DUCKDB_TYPE_BOOLEAN:
		/* Mirror get_col: the widths it returns as a PHP int. UBIGINT/HUGEINT and
		 * wider come back as strings, so they stay PDO_PARAM_STR via the default. */
		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER:
			pdo_type = PDO_PARAM_INT;
			break;
		case DUCKDB_TYPE_BLOB:
			pdo_type = PDO_PARAM_LOB;
			break;
		default:
			pdo_type = PDO_PARAM_STR;
			break;
	}
	add_assoc_long(return_value, "pdo_type", pdo_type);

	/* DECIMAL scale (digits after the point). The total-digit width is reported
	 * as "precision" via describe(), since PDO core overwrites any "precision"
	 * set here from the column struct. */
	if (tid == DUCKDB_TYPE_DECIMAL) {
		add_assoc_long(return_value, "scale", duckdb_decimal_scale(lt));
	}

	return SUCCESS;
}

static int pdo_duckdb_stmt_bind_failure(pdo_duckdb_stmt *S)
{
	S->binds_cleared = false;
	return 0;
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
		return pdo_duckdb_stmt_bind_failure(S);
	}

	/* First bind of this execute round: clear the bindings DuckDB kept from the
	 * previous execute so a param omitted this time isn't reused stale. */
	if (!S->binds_cleared) {
		duckdb_clear_bindings(S->prepared);
		S->binds_cleared = true;
	}

	idx = (idx_t)(param->paramno + 1);

	if (Z_ISREF(param->parameter)) {
		parameter = Z_REFVAL(param->parameter);
	} else {
		parameter = &param->parameter;
	}

	/* Convert a local copy. try_convert_to_string / stream_read / convert_to_long
	 * can re-enter PDOStatement::execute(), which frees stmt->bound_params
	 * (including `param`) under this frame. Never write back through `parameter`. */
	{
		HashTable *bound_at_start = stmt->bound_params;
		zval tmp;
		int bind_ok = 0;

		ZVAL_COPY(&tmp, parameter);
		parameter = &tmp;

		switch (PDO_PARAM_TYPE(param->param_type)) {
			case PDO_PARAM_STMT:
				pdo_duckdb_error_stmt(stmt, "PDO_PARAM_STMT is not supported");
				break;

			case PDO_PARAM_NULL:
				if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
					bind_ok = 1;
				} else {
					pdo_duckdb_error_stmt(stmt, "Failed to bind NULL parameter");
				}
				break;

			case PDO_PARAM_INT:
			case PDO_PARAM_BOOL:
				if (Z_TYPE_P(parameter) == IS_NULL) {
					if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
						bind_ok = 1;
					}
				} else if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_BOOL) {
					bool v = zend_is_true(parameter);
					if (stmt->bound_params == bound_at_start && !EG(exception)
							&& duckdb_bind_boolean(S->prepared, idx, v) == DuckDBSuccess) {
						bind_ok = 1;
					}
				} else {
					convert_to_long(parameter);
					if (stmt->bound_params == bound_at_start && !EG(exception)
							&& duckdb_bind_int64(S->prepared, idx, (int64_t)Z_LVAL_P(parameter)) == DuckDBSuccess) {
						bind_ok = 1;
					}
				}
				if (!bind_ok && !EG(exception)) {
					pdo_duckdb_error_stmt(stmt, "Failed to bind integer parameter");
				}
				break;
			case PDO_PARAM_LOB:
				if (Z_TYPE_P(parameter) == IS_RESOURCE) {
					php_stream *stm = NULL;
					zend_string *mem;

					php_stream_from_zval_no_verify(stm, parameter);
					if (!stm) {
						pdo_raise_impl_error(stmt->dbh, stmt, "HY105", "Expected a stream resource");
						break;
					}
					/* CR-005: cheap size probe first (seekable streams report
					 * their size without reading); otherwise the bounded copy
					 * below stops just past the cap, so a huge stream is never
					 * read whole nor kept. Either way the bind fails with the
					 * cap message through the normal bind-failure path. */
					{
						php_stream_statbuf ssb;

						memset(&ssb, 0, sizeof(ssb));
						if (php_stream_stat(stm, &ssb) == 0 && ssb.sb.st_size > 0
								&& (uint64_t)ssb.sb.st_size > (uint64_t)PDO_DUCKDB_LOB_MAX_BYTES) {
							pdo_duckdb_error_stmt(stmt, "LOB stream exceeds maximum size of 64MB");
							break;
						}
					}
					mem = php_stream_copy_to_mem(stm, PDO_DUCKDB_LOB_MAX_BYTES + 1, 0);
					if (stmt->bound_params != bound_at_start || EG(exception)) {
						if (mem) {
							zend_string_release(mem);
						}
						break;
					}
				if (!mem) {
					/* An exhausted non-seekable stream reads zero bytes: bind
					 * the empty remainder rather than failing the re-execute.
					 * A fresh stream that yields nothing and is not at EOF is
					 * a genuine read failure. */
					if (php_stream_eof(stm)) {
						mem = ZSTR_EMPTY_ALLOC();
					} else {
						pdo_duckdb_error_stmt(stmt, "Failed to read LOB stream");
						break;
					}
				}
					if (ZSTR_LEN(mem) > PDO_DUCKDB_LOB_MAX_BYTES) {
						zend_string_release(mem);
						pdo_duckdb_error_stmt(stmt, "LOB stream exceeds maximum size of 64MB");
						break;
					}
				/* Rewind for the next re-execute; non-seekable streams have no
				 * rewind, so later executes bind from the current position.
				 * Best-effort and result-ignored: user-space wrappers without
				 * stream_seek warn from inside their seek handler, so mute
				 * warnings across the call (PHP 8.4 exports no silence API;
				 * save/restore EG(error_reporting) around this one call). */
				if (!(stm->flags & PHP_STREAM_FLAG_NO_SEEK) && stm->ops->seek) {
					zend_long orig_reporting = EG(error_reporting);
					EG(error_reporting) = 0;
					php_stream_seek(stm, 0, SEEK_SET);
					EG(error_reporting) = orig_reporting;
				}
					if (duckdb_bind_blob(S->prepared, idx, ZSTR_VAL(mem), (idx_t)ZSTR_LEN(mem)) == DuckDBSuccess) {
						bind_ok = 1;
					} else {
						pdo_duckdb_error_stmt(stmt, "Failed to bind blob parameter");
					}
					zend_string_release(mem);
				} else if (Z_TYPE_P(parameter) == IS_NULL) {
					if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
						bind_ok = 1;
					} else {
						pdo_duckdb_error_stmt(stmt, "Failed to bind NULL parameter");
					}
				} else if (try_convert_to_string(parameter) && !EG(exception)
						&& stmt->bound_params == bound_at_start) {
					if (duckdb_bind_blob(S->prepared, idx, Z_STRVAL_P(parameter), (idx_t)Z_STRLEN_P(parameter)) == DuckDBSuccess) {
						bind_ok = 1;
					} else {
						pdo_duckdb_error_stmt(stmt, "Failed to bind blob parameter");
					}
				}
				break;

			case PDO_PARAM_STR:
			default:
				if (Z_TYPE_P(parameter) == IS_NULL) {
					if (duckdb_bind_null(S->prepared, idx) == DuckDBSuccess) {
						bind_ok = 1;
					}
				} else if (try_convert_to_string(parameter) && !EG(exception)
						&& stmt->bound_params == bound_at_start) {
					if (duckdb_bind_varchar_length(S->prepared, idx, Z_STRVAL_P(parameter), (idx_t)Z_STRLEN_P(parameter)) == DuckDBSuccess) {
						bind_ok = 1;
					}
				}
				if (!bind_ok && !EG(exception)) {
					pdo_duckdb_error_stmt(stmt, "Failed to bind string parameter");
				}
				break;
		}

		zval_ptr_dtor(&tmp);
		return bind_ok ? 1 : pdo_duckdb_stmt_bind_failure(S);
	}
}

static int pdo_duckdb_stmt_cursor_closer(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt_reset_result_full(stmt);
	return 1;
}

const struct pdo_stmt_methods duckdb_stmt_methods = {
	.dtor = pdo_duckdb_stmt_dtor,
	.executer = pdo_duckdb_stmt_execute,
	.fetcher = pdo_duckdb_stmt_fetch,
	.describer = pdo_duckdb_stmt_describe,
	.get_col = pdo_duckdb_stmt_get_col,
	.param_hook = pdo_duckdb_stmt_param_hook,
	.get_column_meta = pdo_duckdb_stmt_col_meta,
	.cursor_closer = pdo_duckdb_stmt_cursor_closer,
};
