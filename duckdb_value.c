#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "duckdb_value.h"
#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_variant.h"
#include "ext/json/php_json.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PDO_DUCKDB_VALUE_DEPTH 128

typedef struct {
	duckdb_logical_type type;
	duckdb_value scalar;
	pdd_bridge_value *value;
	pdd_bridge_variant *variant;
	pdd_bridge_bytes bytes[2];
	zval item, key, entry, payload;
} pdo_duckdb_value_frame;

typedef struct {
	pdo_duckdb_value_frame frames[PDO_DUCKDB_VALUE_DEPTH];
	pdd_bridge_error error;
	const char *message;
} pdo_duckdb_value_context;

typedef struct {
	const char *data;
	size_t length;
	size_t position;
} pdo_duckdb_json_input;

static bool pdo_duckdb_value_has_oom(void)
{
#ifdef ZEND_HAS_OOM
	return ZEND_HAS_OOM();
#else
	return false;
#endif
}

static zend_string *pdo_duckdb_value_string_init(const char *data, size_t length)
{
	if (pdo_duckdb_value_has_oom()) return NULL;
#ifdef ZEND_HAS_OOM
	return zend_string_finit(data, length, false, true);
#else
	return zend_string_init(data, length, 0);
#endif
}

static zend_result pdo_duckdb_value_set_string(zval *out, const char *data, size_t length)
{
	zend_string *value = pdo_duckdb_value_string_init(data, length);
	if (!value) return FAILURE;
	ZVAL_STR(out, value);
	return SUCCESS;
}

static void pdo_duckdb_value_frame_clear(pdo_duckdb_value_frame *frame)
{
	if (frame->type) duckdb_destroy_logical_type(&frame->type);
	if (frame->scalar) duckdb_destroy_value(&frame->scalar);
	pdd_bridge_value_destroy(&frame->value);
	pdd_bridge_variant_destroy(&frame->variant);
	pdd_bridge_bytes_free(&frame->bytes[0]);
	pdd_bridge_bytes_free(&frame->bytes[1]);
	if (!Z_ISUNDEF(frame->item)) zval_ptr_dtor(&frame->item);
	if (!Z_ISUNDEF(frame->key)) zval_ptr_dtor(&frame->key);
	if (!Z_ISUNDEF(frame->entry)) zval_ptr_dtor(&frame->entry);
	if (!Z_ISUNDEF(frame->payload)) zval_ptr_dtor(&frame->payload);
	memset(frame, 0, sizeof(*frame));
}

static void pdo_duckdb_value_context_clear(pdo_duckdb_value_context *context)
{
	for (unsigned i = 0; i < PDO_DUCKDB_VALUE_DEPTH; i++) {
		pdo_duckdb_value_frame_clear(&context->frames[i]);
	}
	pdd_bridge_error_free(&context->error);
}

static zend_result pdo_duckdb_value_fail(pdo_duckdb_value_context *context, const char *message)
{
	context->message = message;
	return FAILURE;
}

static zend_result pdo_duckdb_value_depth(pdo_duckdb_value_context *context, unsigned depth)
{
	if (pdo_duckdb_value_has_oom()) return FAILURE;
	return depth < PDO_DUCKDB_VALUE_DEPTH ? SUCCESS :
		pdo_duckdb_value_fail(context, "DuckDB value exceeds the maximum nesting depth (128)");
}

static zend_result pdo_duckdb_value_count(pdo_duckdb_value_context *context, idx_t count)
{
	return count <= HT_MAX_SIZE ? SUCCESS :
		pdo_duckdb_value_fail(context, "DuckDB collection exceeds PHP's array size limit");
}

static void pdo_duckdb_value_hash_insert(HashTable *table, const char *name, size_t length, zval *value)
{
	/* Allocating the key before insertion keeps the array destroyable after OOM. */
	zend_string *key = pdo_duckdb_value_string_init(name, length);
	if (!key) return;
	bool inserted = true;
	zend_try {
#ifdef ZEND_HAS_OOM
		inserted = zend_symtable_fupdate(table, key, value) != HASH_NOMEM;
#else
		zend_symtable_update(table, key, value);
#endif
	} zend_catch {
		zend_string_release(key);
		zend_bailout();
	} zend_end_try();
	zend_string_release(key);
	if (inserted) ZVAL_UNDEF(value);
}

static void pdo_duckdb_value_insert(zval *array, const char *name, size_t length, zval *value)
{
	pdo_duckdb_value_hash_insert(Z_ARRVAL_P(array), name, length, value);
}

static void pdo_duckdb_value_insert_string(zval *array, const char *name, size_t name_length,
		const char *data, size_t length)
{
	zval value;
	if (pdo_duckdb_value_set_string(&value, data, length) == FAILURE) return;
	zend_try {
		pdo_duckdb_value_insert(array, name, name_length, &value);
	} zend_catch {
		zval_ptr_dtor(&value);
		zend_bailout();
	} zend_end_try();
	if (!Z_ISUNDEF(value)) zval_ptr_dtor(&value);
}

static void pdo_duckdb_value_append(zval *array, zval *value)
{
	if (pdo_duckdb_value_has_oom()) return;
#ifdef ZEND_HAS_OOM
	if (zend_hash_next_index_finsert(Z_ARRVAL_P(array), value) == HASH_NOMEM) return;
#else
	zend_hash_next_index_insert(Z_ARRVAL_P(array), value);
#endif
	ZVAL_UNDEF(value);
}

static void pdo_duckdb_value_tag(zval *out, const char *type, size_t length)
{
	array_init_size(out, 2);
	pdo_duckdb_value_insert_string(out, "type", sizeof("type") - 1, type, length);
}

static zend_result pdo_duckdb_value_string(pdo_duckdb_value_context *context,
		pdo_duckdb_value_frame *frame, zval *out)
{
	bool is_null;
	if (pdd_bridge_value_render_pdo(frame->value, NULL, &frame->bytes[1], &is_null,
			&context->error) != PDD_BRIDGE_OK) return FAILURE;
	if (is_null) {
		ZVAL_NULL(out);
	} else {
		return pdo_duckdb_value_set_string(out, frame->bytes[1].data, frame->bytes[1].length);
	}
	return SUCCESS;
}

static zend_result pdo_duckdb_value_geometry(pdo_duckdb_value_context *context,
		const unsigned char *data, size_t length, zval *out)
{
	if (length > ZSTR_MAX_LEN / 2) return pdo_duckdb_value_fail(context, "DuckDB geometry exceeds PHP's string size limit");
	if (pdo_duckdb_value_has_oom()) return FAILURE;
#ifdef ZEND_HAS_OOM
	zend_string *text = zend_string_falloc(length * 2, false, true);
	if (!text) return FAILURE;
#else
	zend_string *text = zend_string_alloc(length * 2, 0);
#endif
	ZVAL_STR(out, text);
	static const char hex[] = "0123456789ABCDEF";
	for (size_t i = 0; i < length; i++) {
		ZSTR_VAL(text)[i * 2] = hex[data[i] >> 4];
		ZSTR_VAL(text)[i * 2 + 1] = hex[data[i] & 15];
	}
	ZSTR_VAL(text)[length * 2] = '\0';
	return SUCCESS;
}

static zend_result pdo_duckdb_value_scalar(pdo_duckdb_value_context *context,
		pdo_duckdb_value_frame *frame, duckdb_type type, zval *out)
{
	switch (type) {
		case DUCKDB_TYPE_BOOLEAN:
			ZVAL_LONG(out, duckdb_get_bool(frame->scalar) ? 1 : 0);
			return SUCCESS;
		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER: {
			int64_t value = duckdb_get_int64(frame->scalar);
			if (value < ZEND_LONG_MIN || value > ZEND_LONG_MAX) {
				return pdo_duckdb_value_string(context, frame, out);
			}
			ZVAL_LONG(out, (zend_long)value);
			return SUCCESS;
		}
		case DUCKDB_TYPE_FLOAT:
			ZVAL_DOUBLE(out, (double)duckdb_get_float(frame->scalar));
			return SUCCESS;
		case DUCKDB_TYPE_DOUBLE:
			ZVAL_DOUBLE(out, duckdb_get_double(frame->scalar));
			return SUCCESS;
		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_BLOB:
			if (pdd_bridge_value_string(frame->value, &frame->bytes[1], &context->error) != PDD_BRIDGE_OK) return FAILURE;
			return pdo_duckdb_value_set_string(out, frame->bytes[1].data, frame->bytes[1].length);
		case DUCKDB_TYPE_GEOMETRY:
			if (pdd_bridge_value_string(frame->value, &frame->bytes[1], &context->error) != PDD_BRIDGE_OK) return FAILURE;
			return pdo_duckdb_value_geometry(context, (const unsigned char *)frame->bytes[1].data, frame->bytes[1].length, out);
		case DUCKDB_TYPE_UBIGINT:
		case DUCKDB_TYPE_HUGEINT:
		case DUCKDB_TYPE_UHUGEINT:
		case DUCKDB_TYPE_DECIMAL:
		case DUCKDB_TYPE_DATE:
		case DUCKDB_TYPE_TIME:
		case DUCKDB_TYPE_TIME_NS:
		case DUCKDB_TYPE_TIME_TZ:
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIMESTAMP_S:
		case DUCKDB_TYPE_TIMESTAMP_MS:
		case DUCKDB_TYPE_TIMESTAMP_NS:
		case DUCKDB_TYPE_TIMESTAMP_TZ:
#ifdef HAVE_DUCKDB_V2
		case DUCKDB_TYPE_TIMESTAMP_TZ_NS:
#endif
		case DUCKDB_TYPE_INTERVAL:
		case DUCKDB_TYPE_UUID:
		case DUCKDB_TYPE_ENUM:
		case DUCKDB_TYPE_BIT:
		case DUCKDB_TYPE_BIGNUM:
			return pdo_duckdb_value_string(context, frame, out);
		default:
			return pdo_duckdb_value_fail(context, "DuckDB scalar type is not supported by this pdo_duckdb build");
	}
}

static void pdo_duckdb_json_space(pdo_duckdb_json_input *input)
{
	while (input->position < input->length) {
		char c = input->data[input->position];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
		input->position++;
	}
}

static zend_result pdo_duckdb_json_string(pdo_duckdb_value_context *context,
		pdo_duckdb_json_input *input, zval *out)
{
	size_t start = input->position++;
	while (input->position < input->length) {
		char c = input->data[input->position++];
		if (c == '\\') {
			if (input->position == input->length) break;
			input->position++;
		} else if (c == '"') {
			/* PHP's JSON scanner requires a NUL sentinel after this token. */
			zend_string *token = pdo_duckdb_value_string_init(input->data + start, input->position - start);
			if (!token) return FAILURE;
			zend_result status;
			zend_try {
				status = php_json_decode_ex(out, ZSTR_VAL(token), ZSTR_LEN(token), 0, 2);
			} zend_catch {
				zend_string_release(token);
				zend_bailout();
			} zend_end_try();
			zend_string_release(token);
			if (status == SUCCESS && Z_TYPE_P(out) == IS_STRING) return SUCCESS;
			return pdo_duckdb_value_fail(context, "DuckDB JSON contains an invalid string");
		}
	}
	return pdo_duckdb_value_fail(context, "DuckDB JSON contains an unterminated string");
}

static bool pdo_duckdb_json_digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool pdo_duckdb_json_word(const pdo_duckdb_json_input *input, const char *word, size_t length)
{
	if (length > input->length - input->position) return false;
	for (size_t i = 0; i < length; i++) {
		if (((unsigned char)input->data[input->position + i] | 0x20) != (unsigned char)word[i]) return false;
	}
	return true;
}

static zend_result pdo_duckdb_json_number(pdo_duckdb_value_context *context,
		pdo_duckdb_json_input *input, zval *out)
{
	size_t start = input->position;
	if (input->data[input->position] == '-') input->position++;
	if (input->position == input->length) goto invalid;
	if (pdo_duckdb_json_word(input, "nan", sizeof("nan") - 1)) {
		input->position += sizeof("nan") - 1;
		goto parsed;
	}
	if (pdo_duckdb_json_word(input, "infinity", sizeof("infinity") - 1)) {
		input->position += sizeof("infinity") - 1;
		goto parsed;
	}
	if (pdo_duckdb_json_word(input, "inf", sizeof("inf") - 1)) {
		input->position += sizeof("inf") - 1;
		goto parsed;
	}
	if (input->data[input->position] == '0') {
		input->position++;
	} else {
		if (input->data[input->position] < '1' || input->data[input->position] > '9') goto invalid;
		do { input->position++; } while (input->position < input->length && pdo_duckdb_json_digit(input->data[input->position]));
	}
	if (input->position < input->length && input->data[input->position] == '.') {
		input->position++;
		if (input->position == input->length || !pdo_duckdb_json_digit(input->data[input->position])) goto invalid;
		do { input->position++; } while (input->position < input->length && pdo_duckdb_json_digit(input->data[input->position]));
	}
	if (input->position < input->length && (input->data[input->position] == 'e' || input->data[input->position] == 'E')) {
		input->position++;
		if (input->position < input->length && (input->data[input->position] == '+' || input->data[input->position] == '-')) input->position++;
		if (input->position == input->length || !pdo_duckdb_json_digit(input->data[input->position])) goto invalid;
		do { input->position++; } while (input->position < input->length && pdo_duckdb_json_digit(input->data[input->position]));
	}
parsed:
	pdo_duckdb_value_tag(out, "number", sizeof("number") - 1);
	pdo_duckdb_value_insert_string(out, "value", sizeof("value") - 1, input->data + start, input->position - start);
	return SUCCESS;
invalid:
	return pdo_duckdb_value_fail(context, "DuckDB JSON contains an invalid number");
}

static zend_result pdo_duckdb_json_node(pdo_duckdb_value_context *context,
		pdo_duckdb_json_input *input, unsigned depth, zval *out);

static zend_result pdo_duckdb_json_node_impl(pdo_duckdb_value_context *context,
		pdo_duckdb_json_input *input, unsigned depth, zval *out)
{
	pdo_duckdb_value_frame *frame = &context->frames[depth];
	pdo_duckdb_json_space(input);
	if (input->position == input->length) return pdo_duckdb_value_fail(context, "DuckDB JSON ends before its value");
	char c = input->data[input->position];
	if (c == '"') return pdo_duckdb_json_string(context, input, out);
	if (c == '-' || pdo_duckdb_json_digit(c) || pdo_duckdb_json_word(input, "nan", sizeof("nan") - 1)
			|| pdo_duckdb_json_word(input, "inf", sizeof("inf") - 1)) {
		return pdo_duckdb_json_number(context, input, out);
	}
	if (c == '[' || c == '{') {
		bool object = c == '{';
		char end = object ? '}' : ']';
		input->position++;
		array_init(&frame->payload);
		pdo_duckdb_json_space(input);
		if (input->position < input->length && input->data[input->position] == end) {
			input->position++;
		} else {
			for (;;) {
				if (zend_hash_num_elements(Z_ARRVAL(frame->payload)) == HT_MAX_SIZE) {
					return pdo_duckdb_value_fail(context, "DuckDB JSON exceeds PHP's array size limit");
				}
				if (object) {
					pdo_duckdb_json_space(input);
					if (input->position == input->length || input->data[input->position] != '"') goto invalid;
					if (pdo_duckdb_json_string(context, input, &frame->key) == FAILURE) return FAILURE;
					pdo_duckdb_json_space(input);
					if (input->position == input->length || input->data[input->position++] != ':') goto invalid;
				}
				if (pdo_duckdb_json_node(context, input, depth + 1, &frame->item) == FAILURE) return FAILURE;
				if (object) {
					array_init_size(&frame->entry, 2);
					pdo_duckdb_value_insert(&frame->entry, "key", sizeof("key") - 1, &frame->key);
					pdo_duckdb_value_insert(&frame->entry, "value", sizeof("value") - 1, &frame->item);
					pdo_duckdb_value_append(&frame->payload, &frame->entry);
				} else {
					pdo_duckdb_value_append(&frame->payload, &frame->item);
				}
				pdo_duckdb_json_space(input);
				if (input->position == input->length) goto invalid;
				c = input->data[input->position++];
				if (c == end) break;
				if (c != ',') goto invalid;
			}
		}
		if (object) {
			pdo_duckdb_value_tag(out, "object", sizeof("object") - 1);
			pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->payload);
		} else {
			ZVAL_COPY_VALUE(out, &frame->payload);
			ZVAL_UNDEF(&frame->payload);
		}
		return SUCCESS;
	}
	if (input->length - input->position >= 4 && memcmp(input->data + input->position, "null", 4) == 0) {
		input->position += 4;
		ZVAL_NULL(out);
		return SUCCESS;
	}
	if (input->length - input->position >= 4 && memcmp(input->data + input->position, "true", 4) == 0) {
		input->position += 4;
		ZVAL_TRUE(out);
		return SUCCESS;
	}
	if (input->length - input->position >= 5 && memcmp(input->data + input->position, "false", 5) == 0) {
		input->position += 5;
		ZVAL_FALSE(out);
		return SUCCESS;
	}
invalid:
	return pdo_duckdb_value_fail(context, "DuckDB JSON contains invalid syntax");
}

static zend_result pdo_duckdb_json_node(pdo_duckdb_value_context *context,
		pdo_duckdb_json_input *input, unsigned depth, zval *out)
{
	if (pdo_duckdb_value_depth(context, depth) == FAILURE) return FAILURE;
	zend_result status = pdo_duckdb_json_node_impl(context, input, depth, out);
	pdo_duckdb_value_frame_clear(&context->frames[depth]);
	return pdo_duckdb_value_has_oom() ? FAILURE : status;
}

static zend_result pdo_duckdb_variant_native(pdo_duckdb_value_context *context,
		const pdd_bridge_variant *node, unsigned depth, zval *out);

static zend_result pdo_duckdb_variant_native_impl(pdo_duckdb_value_context *context,
		const pdd_bridge_variant *node, unsigned depth, zval *out)
{
	pdo_duckdb_value_frame *frame = &context->frames[depth];
	pdd_bridge_variant_kind kind;
	idx_t count;
	if (pdd_bridge_variant_info(node, &kind, &count, &context->error) != PDD_BRIDGE_OK) return FAILURE;
	if (kind == PDD_BRIDGE_VARIANT_SQL_NULL) {
		ZVAL_NULL(out);
		return SUCCESS;
	}
	if (kind == PDD_BRIDGE_VARIANT_NULL) {
		pdo_duckdb_value_tag(out, "NULL", sizeof("NULL") - 1);
		ZVAL_NULL(&frame->item);
		pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->item);
		return SUCCESS;
	}
	if (kind == PDD_BRIDGE_VARIANT_SCALAR) {
		if (pdd_bridge_variant_scalar(node, &frame->scalar, &context->error) != PDD_BRIDGE_OK
				|| pdd_bridge_value_from_c(frame->scalar, &frame->value, &context->error) != PDD_BRIDGE_OK
				|| pdd_bridge_value_type(frame->value, &frame->type, &context->error) != PDD_BRIDGE_OK
				|| pdd_bridge_type_declaration(frame->type, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
		pdo_duckdb_value_tag(out, frame->bytes[0].data, frame->bytes[0].length);
		if (pdo_duckdb_value_scalar(context, frame, duckdb_get_type_id(frame->type), &frame->item) == FAILURE) return FAILURE;
		pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->item);
		return SUCCESS;
	}
	if (pdo_duckdb_value_count(context, count) == FAILURE) return FAILURE;
	bool object = kind == PDD_BRIDGE_VARIANT_OBJECT;
	pdo_duckdb_value_tag(out, object ? "OBJECT" : "ARRAY", object ? sizeof("OBJECT") - 1 : sizeof("ARRAY") - 1);
	array_init_size(&frame->payload, (uint32_t)count);
	for (idx_t i = 0; i < count; i++) {
		if (pdd_bridge_variant_child(node, i, &frame->variant, &context->error) != PDD_BRIDGE_OK) return FAILURE;
		if (pdo_duckdb_variant_native(context, frame->variant, depth + 1, &frame->item) == FAILURE) return FAILURE;
		pdd_bridge_variant_destroy(&frame->variant);
		if (object) {
			if (pdd_bridge_variant_key(node, i, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
			array_init_size(&frame->entry, 2);
			pdo_duckdb_value_insert_string(&frame->entry, "key", sizeof("key") - 1, frame->bytes[0].data, frame->bytes[0].length);
			pdd_bridge_bytes_free(&frame->bytes[0]);
			pdo_duckdb_value_insert(&frame->entry, "value", sizeof("value") - 1, &frame->item);
			pdo_duckdb_value_append(&frame->payload, &frame->entry);
		} else {
			pdo_duckdb_value_append(&frame->payload, &frame->item);
		}
	}
	pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->payload);
	return SUCCESS;
}

static zend_result pdo_duckdb_variant_native(pdo_duckdb_value_context *context,
		const pdd_bridge_variant *node, unsigned depth, zval *out)
{
	if (pdo_duckdb_value_depth(context, depth) == FAILURE) return FAILURE;
	zend_result status = pdo_duckdb_variant_native_impl(context, node, depth, out);
	pdo_duckdb_value_frame_clear(&context->frames[depth]);
	return pdo_duckdb_value_has_oom() ? FAILURE : status;
}

static zend_result pdo_duckdb_value_native_at(pdo_duckdb_value_context *context, duckdb_vector vector,
		idx_t row, idx_t row_count, unsigned depth, zval *out);

static zend_result pdo_duckdb_value_native_impl(pdo_duckdb_value_context *context, duckdb_vector vector,
		idx_t row, idx_t row_count, unsigned depth, zval *out)
{
	pdo_duckdb_value_frame *frame = &context->frames[depth];
	if (!vector || row >= row_count) return pdo_duckdb_value_fail(context, "DuckDB value row is out of bounds");
	uint64_t *validity = duckdb_vector_get_validity(vector);
	if (validity && !duckdb_validity_row_is_valid(validity, row)) {
		ZVAL_NULL(out);
		return SUCCESS;
	}
	frame->type = duckdb_vector_get_column_type(vector);
	if (!frame->type) return pdo_duckdb_value_fail(context, "DuckDB value has no logical type");
	duckdb_type type = duckdb_get_type_id(frame->type);
	if (type == DUCKDB_TYPE_SQLNULL) {
		ZVAL_NULL(out);
		return SUCCESS;
	}
	if (type == DUCKDB_TYPE_VARIANT) {
		if (pdd_bridge_variant_from_vector(vector, row, row_count, &frame->variant, &context->error) != PDD_BRIDGE_OK) return FAILURE;
		return pdo_duckdb_variant_native(context, frame->variant, depth + 1, out);
	}
	if (type == DUCKDB_TYPE_VARCHAR) {
		if (pdd_bridge_type_alias(frame->type, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
		if (frame->bytes[0].length == sizeof("JSON") - 1 && memcmp(frame->bytes[0].data, "JSON", sizeof("JSON") - 1) == 0) {
			duckdb_string_t text = ((duckdb_string_t *)duckdb_vector_get_data(vector))[row];
			pdo_duckdb_json_input input = {duckdb_string_t_data(&text), (size_t)duckdb_string_t_length(text), 0};
			pdo_duckdb_value_tag(out, "JSON", sizeof("JSON") - 1);
			if (pdo_duckdb_json_node(context, &input, depth + 1, &frame->item) == FAILURE) return FAILURE;
			pdo_duckdb_json_space(&input);
			if (input.position != input.length) return pdo_duckdb_value_fail(context, "DuckDB JSON has trailing data");
			pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->item);
			return SUCCESS;
		}
		pdd_bridge_bytes_free(&frame->bytes[0]);
	}
	if (type == DUCKDB_TYPE_GEOMETRY) {
		duckdb_string_t bytes = ((duckdb_string_t *)duckdb_vector_get_data(vector))[row];
		return pdo_duckdb_value_geometry(context, (const unsigned char *)duckdb_string_t_data(&bytes),
			duckdb_string_t_length(bytes), out);
	}
	if (type == DUCKDB_TYPE_LIST || type == DUCKDB_TYPE_ARRAY || type == DUCKDB_TYPE_MAP) {
		idx_t offset, count, child_count;
		duckdb_vector child;
		if (type == DUCKDB_TYPE_ARRAY) {
			count = duckdb_array_type_array_size(frame->type);
			if (count && row_count > UINT64_MAX / count) return pdo_duckdb_value_fail(context, "DuckDB array size overflows");
			offset = row * count;
			child_count = row_count * count;
			child = duckdb_array_vector_get_child(vector);
		} else {
			duckdb_list_entry entry = ((duckdb_list_entry *)duckdb_vector_get_data(vector))[row];
			offset = entry.offset;
			count = entry.length;
			child_count = duckdb_list_vector_get_size(vector);
			if (offset > child_count || count > child_count - offset) return pdo_duckdb_value_fail(context, "DuckDB list range is out of bounds");
			child = duckdb_list_vector_get_child(vector);
		}
		if (pdo_duckdb_value_count(context, count) == FAILURE) return FAILURE;
		array_init_size(out, (uint32_t)count);
		for (idx_t i = 0; i < count; i++) {
			if (type == DUCKDB_TYPE_MAP) {
				if (pdo_duckdb_value_native_at(context, duckdb_struct_vector_get_child(child, 0), offset + i,
						child_count, depth + 1, &frame->key) == FAILURE
						|| pdo_duckdb_value_native_at(context, duckdb_struct_vector_get_child(child, 1), offset + i,
						child_count, depth + 1, &frame->item) == FAILURE) return FAILURE;
				array_init_size(&frame->entry, 2);
				pdo_duckdb_value_insert(&frame->entry, "key", sizeof("key") - 1, &frame->key);
				pdo_duckdb_value_insert(&frame->entry, "value", sizeof("value") - 1, &frame->item);
				pdo_duckdb_value_append(out, &frame->entry);
			} else {
				if (pdo_duckdb_value_native_at(context, child, offset + i, child_count, depth + 1, &frame->item) == FAILURE) return FAILURE;
				pdo_duckdb_value_append(out, &frame->item);
			}
		}
		return SUCCESS;
	}
	if (type == DUCKDB_TYPE_STRUCT) {
		bool tuple;
		idx_t count = duckdb_struct_type_child_count(frame->type);
		if (pdo_duckdb_value_count(context, count) == FAILURE
				|| pdd_bridge_type_is_tuple(frame->type, &tuple, &context->error) != PDD_BRIDGE_OK) return FAILURE;
		array_init_size(out, (uint32_t)count);
		for (idx_t i = 0; i < count; i++) {
			if (pdo_duckdb_value_native_at(context, duckdb_struct_vector_get_child(vector, i), row,
					row_count, depth + 1, &frame->item) == FAILURE) return FAILURE;
			if (tuple) {
				pdo_duckdb_value_append(out, &frame->item);
			} else {
				if (pdd_bridge_type_field_name(frame->type, i, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
				pdo_duckdb_value_insert(out, frame->bytes[0].data, frame->bytes[0].length, &frame->item);
				pdd_bridge_bytes_free(&frame->bytes[0]);
			}
		}
		return SUCCESS;
	}
	if (type == DUCKDB_TYPE_UNION) {
		duckdb_vector tags = duckdb_struct_vector_get_child(vector, 0);
		idx_t tag = ((uint8_t *)duckdb_vector_get_data(tags))[row];
		if (tag >= duckdb_union_type_member_count(frame->type)) return pdo_duckdb_value_fail(context, "DuckDB union tag is out of bounds");
		if (pdd_bridge_type_field_name(frame->type, tag, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
		array_init_size(out, 2);
		pdo_duckdb_value_insert_string(out, "tag", sizeof("tag") - 1, frame->bytes[0].data, frame->bytes[0].length);
		if (pdo_duckdb_value_native_at(context, duckdb_struct_vector_get_child(vector, tag + 1), row,
				row_count, depth + 1, &frame->item) == FAILURE) return FAILURE;
		pdo_duckdb_value_insert(out, "value", sizeof("value") - 1, &frame->item);
		return SUCCESS;
	}
	if (pdd_bridge_value_from_vector(vector, row, row_count, &frame->value, &context->error) != PDD_BRIDGE_OK
			|| pdd_bridge_value_to_c(frame->value, &frame->scalar, &context->error) != PDD_BRIDGE_OK) return FAILURE;
	return pdo_duckdb_value_scalar(context, frame, type, out);
}

static zend_result pdo_duckdb_value_native_at(pdo_duckdb_value_context *context, duckdb_vector vector,
		idx_t row, idx_t row_count, unsigned depth, zval *out)
{
	if (pdo_duckdb_value_depth(context, depth) == FAILURE) return FAILURE;
	zend_result status = pdo_duckdb_value_native_impl(context, vector, row, row_count, depth, out);
	pdo_duckdb_value_frame_clear(&context->frames[depth]);
	return pdo_duckdb_value_has_oom() ? FAILURE : status;
}

static zend_result pdo_duckdb_value_wrap_column(pdo_duckdb_value_context *context,
		duckdb_vector vector, zval *out)
{
	if (Z_TYPE_P(out) == IS_NULL) return SUCCESS;
	pdo_duckdb_value_frame *frame = &context->frames[0];
	frame->type = duckdb_vector_get_column_type(vector);
	if (!frame->type) return pdo_duckdb_value_fail(context, "DuckDB value has no logical type");
	bool tagged = false;
	switch (duckdb_get_type_id(frame->type)) {
		case DUCKDB_TYPE_VARIANT:
			tagged = true;
			break;
		case DUCKDB_TYPE_VARCHAR:
			if (pdd_bridge_type_alias(frame->type, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
			if (frame->bytes[0].length != sizeof("JSON") - 1
					|| memcmp(frame->bytes[0].data, "JSON", sizeof("JSON") - 1) != 0) return SUCCESS;
			tagged = true;
			break;
		case DUCKDB_TYPE_LIST:
		case DUCKDB_TYPE_ARRAY:
		case DUCKDB_TYPE_STRUCT:
		case DUCKDB_TYPE_MAP:
		case DUCKDB_TYPE_UNION:
			break;
		default:
			return SUCCESS;
	}
	ZVAL_COPY_VALUE(&frame->payload, out);
	ZVAL_UNDEF(out);
	if (tagged) {
		if (Z_TYPE(frame->payload) != IS_ARRAY) return pdo_duckdb_value_fail(context, "DuckDB tagged value has an invalid representation");
		zval *type = zend_hash_str_find(Z_ARRVAL(frame->payload), "type", sizeof("type") - 1);
		zval *value = zend_hash_str_find(Z_ARRVAL(frame->payload), "value", sizeof("value") - 1);
		if (!type || Z_TYPE_P(type) != IS_STRING || !value) return pdo_duckdb_value_fail(context, "DuckDB tagged value has an invalid representation");
		ZVAL_COPY_VALUE(&frame->key, type);
		ZVAL_UNDEF(type);
		ZVAL_COPY_VALUE(&frame->item, value);
		ZVAL_UNDEF(value);
	} else {
		if (pdd_bridge_type_declaration(frame->type, &frame->bytes[0], &context->error) != PDD_BRIDGE_OK) return FAILURE;
		if (pdo_duckdb_value_set_string(&frame->key, frame->bytes[0].data, frame->bytes[0].length) == FAILURE) return FAILURE;
		ZVAL_COPY_VALUE(&frame->item, &frame->payload);
		ZVAL_UNDEF(&frame->payload);
	}
	object_init(out);
	HashTable *properties = zend_std_get_properties(Z_OBJ_P(out));
	pdo_duckdb_value_hash_insert(properties, "type", sizeof("type") - 1, &frame->key);
	pdo_duckdb_value_hash_insert(properties, "value", sizeof("value") - 1, &frame->item);
	return SUCCESS;
}

static zend_result pdo_duckdb_value_convert(duckdb_vector vector, idx_t row, idx_t row_count,
		bool native, bool column, zval *out, zend_string **error)
{
	ZVAL_UNDEF(out);
	*error = NULL;
	if (pdo_duckdb_value_has_oom()) return FAILURE;
	pdo_duckdb_value_context *context = calloc(1, sizeof(*context));
	if (!context) {
		*error = zend_string_init("Unable to allocate DuckDB value conversion state", sizeof("Unable to allocate DuckDB value conversion state") - 1, 0);
		return FAILURE;
	}
	zend_result status = FAILURE;
	zend_try {
		if (native) {
			status = pdo_duckdb_value_native_at(context, vector, row, row_count, 0, out);
			if (status == SUCCESS && column) status = pdo_duckdb_value_wrap_column(context, vector, out);
		} else {
			pdo_duckdb_value_frame *frame = &context->frames[0];
			if (pdd_bridge_value_from_vector(vector, row, row_count, &frame->value, &context->error) == PDD_BRIDGE_OK) {
				status = pdo_duckdb_value_string(context, frame, out);
			}
		}
		if (pdo_duckdb_value_has_oom()) status = FAILURE;
		if (status == FAILURE && !pdo_duckdb_value_has_oom()) {
			const char *message = context->error.data ? context->error.data : context->message;
			size_t length = context->error.data ? context->error.length : (message ? strlen(message) : 0);
			if (!message) {
				message = "Unable to convert DuckDB value";
				length = sizeof("Unable to convert DuckDB value") - 1;
			}
			*error = zend_string_init(message, length, 0);
		}
	} zend_catch {
		pdo_duckdb_value_context_clear(context);
		free(context);
		if (!Z_ISUNDEF_P(out)) {
			zval_ptr_dtor(out);
			ZVAL_UNDEF(out);
		}
		zend_bailout();
	} zend_end_try();
	pdo_duckdb_value_context_clear(context);
	free(context);
	if (status == FAILURE && !Z_ISUNDEF_P(out)) {
		zval_ptr_dtor(out);
		ZVAL_UNDEF(out);
	}
	return status;
}

zend_result pdo_duckdb_value_native(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error)
{
	return pdo_duckdb_value_convert(vector, row, row_count, true, false, out, error);
}

zend_result pdo_duckdb_value_native_column(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error)
{
	return pdo_duckdb_value_convert(vector, row, row_count, true, true, out, error);
}

zend_result pdo_duckdb_value_render(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error)
{
	return pdo_duckdb_value_convert(vector, row, row_count, false, false, out, error);
}

zend_result pdo_duckdb_value_is_structured(duckdb_logical_type type, bool *out, zend_string **error)
{
	*out = false;
	*error = NULL;
	switch (duckdb_get_type_id(type)) {
		case DUCKDB_TYPE_LIST:
		case DUCKDB_TYPE_ARRAY:
		case DUCKDB_TYPE_STRUCT:
		case DUCKDB_TYPE_MAP:
		case DUCKDB_TYPE_UNION:
		case DUCKDB_TYPE_VARIANT:
			*out = true;
			return SUCCESS;
		case DUCKDB_TYPE_VARCHAR: {
			pdd_bridge_bytes alias = {0};
			pdd_bridge_error bridge_error = {0};
			pdd_bridge_status status = pdd_bridge_type_alias(type, &alias, &bridge_error);
			*out = status == PDD_BRIDGE_OK && alias.length == sizeof("JSON") - 1
				&& memcmp(alias.data, "JSON", sizeof("JSON") - 1) == 0;
			zend_try {
				if (status != PDD_BRIDGE_OK) {
					*error = zend_string_init(bridge_error.data ? bridge_error.data : "Unable to inspect DuckDB type",
						bridge_error.data ? bridge_error.length : sizeof("Unable to inspect DuckDB type") - 1, 0);
				}
			} zend_catch {
				pdd_bridge_bytes_free(&alias);
				pdd_bridge_error_free(&bridge_error);
				zend_bailout();
			} zend_end_try();
			pdd_bridge_bytes_free(&alias);
			pdd_bridge_error_free(&bridge_error);
			return status == PDD_BRIDGE_OK ? SUCCESS : FAILURE;
		}
		default:
			return SUCCESS;
	}
}
