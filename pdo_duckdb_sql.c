#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "Zend/zend_smart_str.h"
#include "pdo_duckdb_sql.h"

static bool pdo_duckdb_sql_identifier_start(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static bool pdo_duckdb_sql_identifier_continue(unsigned char c)
{
	return pdo_duckdb_sql_identifier_start(c) || (c >= '0' && c <= '9') || c == '$';
}

static bool pdo_duckdb_sql_parameter_char(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_';
}

static size_t pdo_duckdb_sql_space_length(const char *text, size_t length)
{
	const unsigned char *s = (const unsigned char *)text;
	if (length >= 2 && s[0] == 0xc2 && s[1] == 0xa0) {
		return 2;
	}
	if (length >= 3) {
		if (s[0] == 0xe2 && s[1] == 0x80 &&
				((s[2] >= 0x80 && s[2] <= 0x8a) || s[2] == 0xaf)) {
			return 3;
		}
		if ((s[0] == 0xe2 && s[1] == 0x81 && s[2] == 0x9f) ||
				(s[0] == 0xe3 && s[1] == 0x80 && s[2] == 0x80) ||
				(s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf)) {
			return 3;
		}
	}
	return 0;
}

static size_t pdo_duckdb_sql_skip_string(const char *sql, size_t length,
	size_t start, unsigned char quote, bool escapes)
{
	size_t pos = start + 1;
	while (pos < length) {
		if (escapes && sql[pos] == '\\' && pos + 1 < length) {
			pos += 2;
		} else if ((unsigned char)sql[pos] == quote) {
			if (pos + 1 < length && (unsigned char)sql[pos + 1] == quote) {
				pos += 2;
			} else {
				return pos + 1;
			}
		} else {
			pos++;
		}
	}
	return length;
}

static size_t pdo_duckdb_sql_skip_trivia(const char *sql, size_t length, size_t pos)
{
	while (pos < length) {
		size_t space = pdo_duckdb_sql_space_length(sql + pos, length - pos);
		if (space) {
			pos += space;
		} else if (sql[pos] == ' ' || sql[pos] == '\t' || sql[pos] == '\n' ||
				sql[pos] == '\r' || sql[pos] == '\f' || sql[pos] == '\v') {
			pos++;
		} else if (pos + 1 < length && sql[pos] == '-' && sql[pos + 1] == '-') {
			pos += 2;
			while (pos < length && sql[pos] != '\r' && sql[pos] != '\n') {
				pos++;
			}
		} else if (pos + 1 < length && sql[pos] == '/' && sql[pos + 1] == '*') {
			size_t depth = 1;
			pos += 2;
			while (pos < length && depth) {
				if (pos + 1 < length && sql[pos] == '/' && sql[pos + 1] == '*') {
					depth++;
					pos += 2;
				} else if (pos + 1 < length && sql[pos] == '*' && sql[pos + 1] == '/') {
					depth--;
					pos += 2;
				} else {
					pos++;
				}
			}
		} else {
			break;
		}
	}
	return pos;
}

static size_t pdo_duckdb_sql_skip_dollar_string(const char *sql, size_t length, size_t start)
{
	size_t end = start + 1;
	if (end < length && sql[end] != '$') {
		if (!pdo_duckdb_sql_identifier_start((unsigned char)sql[end])) {
			return start;
		}
		end++;
		while (end < length && (pdo_duckdb_sql_identifier_start((unsigned char)sql[end]) ||
				(sql[end] >= '0' && sql[end] <= '9'))) {
			end++;
		}
	}
	if (end == length || sql[end] != '$') {
		return start;
	}
	size_t delimiter_length = end - start + 1;
	for (size_t pos = end + 1; pos <= length - delimiter_length; pos++) {
		if (sql[pos] == '$' && memcmp(sql + pos, sql + start, delimiter_length) == 0) {
			return pos + delimiter_length;
		}
	}
	return length;
}

static bool pdo_duckdb_sql_add_placeholder(pdo_duckdb_sql_plan *plan,
	size_t offset, size_t length, pdo_duckdb_sql_param_style style, zend_string **error)
{
	if (plan->style != PDO_DUCKDB_SQL_PARAMS_NONE && plan->style != style) {
		*error = zend_string_init("mixed named and positional parameters",
			sizeof("mixed named and positional parameters") - 1, 0);
		return false;
	}
	plan->style = style;
	size_t index = plan->parameter_count;
	zval *found = style == PDO_DUCKDB_SQL_PARAMS_NAMED
		? zend_hash_str_find(&plan->names, ZSTR_VAL(plan->sql) + offset, length) : NULL;
	if (found) {
		index = (size_t)Z_LVAL_P(found);
	} else {
		if (plan->parameter_count == (size_t)ZEND_LONG_MAX) {
			*error = zend_string_init("Too many SQL parameters", sizeof("Too many SQL parameters") - 1, 0);
			return false;
		}
		plan->parameter_count++;
		if (style == PDO_DUCKDB_SQL_PARAMS_NAMED) {
			zval value;
			ZVAL_LONG(&value, (zend_long)index);
			zend_hash_str_add_new(&plan->names, ZSTR_VAL(plan->sql) + offset, length, &value);
		}
	}
	if (plan->occurrence_count == plan->occurrence_capacity) {
		size_t capacity = plan->occurrence_capacity ? plan->occurrence_capacity * 2 : 8;
		if (capacity < plan->occurrence_capacity) {
			zend_error_noreturn(E_ERROR, "Too many SQL parameter occurrences");
		}
		plan->placeholders = safe_erealloc(plan->placeholders, capacity, sizeof(*plan->placeholders), 0);
		plan->occurrence_capacity = capacity;
	}
	plan->placeholders[plan->occurrence_count++] = (pdo_duckdb_sql_placeholder){offset, length, index, true};
	if (index >= plan->native_next_position) {
		plan->native_next_position = index + 1;
	}
	return true;
}

static void pdo_duckdb_sql_add_native_placeholder(pdo_duckdb_sql_plan *plan,
		size_t offset, size_t length, const char *name, size_t name_length, bool numeric, unsigned char quote)
{
	zend_string *key = zend_string_init(name, name_length, 0);
	zend_ulong position = 0;
	zval *found, slot;
	size_t index;
	bool anonymous = false;

	if (quote) {
		size_t decoded = 0;
		for (size_t i = 0; i < name_length; i++) {
			ZSTR_VAL(key)[decoded++] = name[i];
			if ((unsigned char)name[i] == quote && i + 1 < name_length && name[i + 1] == name[i]) {
				i++;
			}
		}
		ZSTR_VAL(key)[decoded] = '\0';
		ZSTR_LEN(key) = decoded;
	}
	zend_str_tolower(ZSTR_VAL(key), ZSTR_LEN(key));
	if (numeric) {
		for (size_t i = 0; i < name_length; i++) {
			if (name[i] == '_') {
				if (plan->legacy_parser) {
					break;
				}
				continue;
			}
			unsigned char digit = (unsigned char)name[i] - '0';
			if (digit > 9 || position > (ZEND_ULONG_MAX - digit) / 10) {
				zend_string_release(key);
				return;
			}
			position = position * 10 + digit;
		}
		if (!position && plan->legacy_parser) {
			position = (zend_ulong)plan->native_next_position;
			anonymous = true;
			if (position > ZEND_LONG_MAX) {
				zend_string_release(key);
				return;
			}
		} else if (!position || position - 1 > ZEND_LONG_MAX) {
			zend_string_release(key);
			return;
		} else {
			position--;
		}
	}
	found = anonymous ? NULL : zend_hash_find(&plan->native_names, key);
	if (found) {
		position = (zend_ulong)Z_LVAL_P(found);
	} else {
		if (!numeric) {
			position = (zend_ulong)plan->native_parameter_count;
		}
		found = zend_hash_index_find(&plan->native_positions, position);
		index = found ? (size_t)Z_LVAL_P(found) : plan->native_parameter_count++;
		if (!anonymous) {
			ZVAL_LONG(&slot, (zend_long)position);
			zend_hash_add_new(&plan->native_names, key, &slot);
		}
		if (!found) {
			ZVAL_LONG(&slot, (zend_long)index);
			zend_hash_index_add_new(&plan->native_positions, position, &slot);
		}
	}
	if (position >= plan->native_next_position) {
		plan->native_next_position = (size_t)position + 1;
	}
	zend_string_release(key);
	if (plan->native_occurrence_count == plan->native_occurrence_capacity) {
		size_t capacity = plan->native_occurrence_capacity ? plan->native_occurrence_capacity * 2 : 8;
		if (capacity < plan->native_occurrence_capacity) {
			zend_error_noreturn(E_ERROR, "Too many native SQL parameter occurrences");
		}
		plan->native_placeholders = safe_erealloc(plan->native_placeholders,
			capacity, sizeof(*plan->native_placeholders), 0);
		plan->native_occurrence_capacity = capacity;
	}
	plan->native_placeholders[plan->native_occurrence_count++] =
		(pdo_duckdb_sql_placeholder){offset, length, (size_t)position, numeric};
}

pdo_duckdb_sql_plan *pdo_duckdb_sql_plan_create(zend_string *sql, bool legacy_parser, zend_string **error)
{
	const char *text = ZSTR_VAL(sql);
	size_t length = ZSTR_LEN(sql);
	size_t pos = 0;
	bool seen_token = false;
	pdo_duckdb_sql_plan *plan;

	*error = NULL;
	if (memchr(text, '\0', length)) {
		*error = zend_string_init("SQL statement contains a NUL byte",
			sizeof("SQL statement contains a NUL byte") - 1, 0);
		return NULL;
	}
	plan = ecalloc(1, sizeof(*plan));
	plan->sql = zend_string_copy(sql);
	plan->legacy_parser = legacy_parser;
	zend_hash_init(&plan->names, 8, NULL, NULL, 0);
	zend_hash_init(&plan->native_names, 8, NULL, NULL, 0);
	zend_hash_init(&plan->native_positions, 8, NULL, NULL, 0);
	while (pos < length) {
		unsigned char c = (unsigned char)text[pos];
		size_t space_length = pdo_duckdb_sql_space_length(text + pos, length - pos);
		if (space_length) {
			pos += space_length;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			pos++;
			continue;
		}
		if (c == ';' && !seen_token) {
			pos++;
			continue;
		}
		if (c == '-' && pos + 1 < length && text[pos + 1] == '-') {
			pos += 2;
			while (pos < length && text[pos] != '\r' && text[pos] != '\n') {
				pos++;
			}
			continue;
		}
		if (c == '/' && pos + 1 < length && text[pos + 1] == '*') {
			size_t depth = 1;
			pos += 2;
			while (pos < length && depth) {
				if (pos + 1 < length && text[pos] == '/' && text[pos + 1] == '*') {
					depth++;
					pos += 2;
				} else if (pos + 1 < length && text[pos] == '*' && text[pos + 1] == '/') {
					depth--;
					pos += 2;
				} else {
					pos++;
				}
			}
			continue;
		}
		if ((c == 'e' || c == 'E') && pos + 1 < length && text[pos + 1] == '\'') {
			pos = pdo_duckdb_sql_skip_string(text, length, pos + 1, '\'', true);
		} else if (c == '\'' || c == '"'
#ifdef HAVE_DUCKDB_V2
				|| c == '`'
#endif
				) {
			pos = pdo_duckdb_sql_skip_string(text, length, pos, c, false);
		} else if (c == '$') {
			size_t end = pdo_duckdb_sql_skip_dollar_string(text, length, pos);
			if (end != pos) {
				pos = end;
			} else {
				size_t start = pos++;
				size_t name_start, name_end;
				unsigned char quote = 0;
				bool numeric;
				plan->has_dollar_parameters = true;
				/* A high byte adjoining '$' belongs to the name, even for Unicode whitespace. */
				if (pos < length && (unsigned char)text[pos] < 0x80) {
					pos = pdo_duckdb_sql_skip_trivia(text, length, pos);
				}
				name_start = pos;
				numeric = pos < length && text[pos] >= '0' && text[pos] <= '9';
				if (numeric) {
					while (pos < length) {
						if (text[pos] >= '0' && text[pos] <= '9') {
							pos++;
						} else if (text[pos] == '_' && pos > name_start && pos + 1 < length &&
								text[pos - 1] >= '0' && text[pos - 1] <= '9' &&
								text[pos + 1] >= '0' && text[pos + 1] <= '9') {
							pos++;
						} else {
							break;
						}
					}
					name_end = pos;
				} else if (pos < length && (text[pos] == '"'
#ifdef HAVE_DUCKDB_V2
						|| text[pos] == '`'
#endif
						)) {
					quote = (unsigned char)text[pos];
					pos = pdo_duckdb_sql_skip_string(text, length, pos, quote, false);
					name_start++;
					name_end = pos && (unsigned char)text[pos - 1] == quote ? pos - 1 : pos;
				} else {
					while (pos < length && (pdo_duckdb_sql_parameter_char((unsigned char)text[pos]) ||
							(unsigned char)text[pos] >= 0x80)) {
						pos++;
					}
					name_end = pos;
				}
				if (name_end > name_start) {
					if (!numeric) {
						plan->has_named_dollar_parameters = true;
					}
					pdo_duckdb_sql_add_native_placeholder(plan, start, pos - start,
						text + name_start, name_end - name_start, numeric, quote);
				}
			}
		} else if (pdo_duckdb_sql_identifier_start(c)) {
			size_t start = pos++;
			while (pos < length && pdo_duckdb_sql_identifier_continue((unsigned char)text[pos])) {
				pos++;
			}
			if (!seen_token && pos - start == sizeof("ATTACH") - 1 &&
					zend_binary_strcasecmp(text + start, pos - start, "ATTACH", sizeof("ATTACH") - 1) == 0) {
				plan->is_attach = true;
			}
		} else if (c == ':' && pos + 1 < length && text[pos + 1] == ':') {
			do {
				pos++;
			} while (pos < length && text[pos] == ':');
		} else if (c == ':' && pos + 1 < length &&
				pdo_duckdb_sql_parameter_char((unsigned char)text[pos + 1]) &&
				(pos == 0 || !pdo_duckdb_sql_identifier_continue((unsigned char)text[pos - 1]))) {
			size_t start = pos++;
			while (pos < length && pdo_duckdb_sql_parameter_char((unsigned char)text[pos])) {
				pos++;
			}
			if (!pdo_duckdb_sql_add_placeholder(plan, start, pos - start, PDO_DUCKDB_SQL_PARAMS_NAMED, error)) {
				pdo_duckdb_sql_plan_destroy(plan);
				return NULL;
			}
		} else if (c == '?' && pos + 1 < length && text[pos + 1] == '?') {
			do {
				pos++;
			} while (pos < length && text[pos] == '?');
		} else if (c == '?') {
			if (!pdo_duckdb_sql_add_placeholder(plan, pos, 1, PDO_DUCKDB_SQL_PARAMS_POSITIONAL, error)) {
				pdo_duckdb_sql_plan_destroy(plan);
				return NULL;
			}
			pos++;
		} else {
			pos++;
		}
		seen_token = true;
	}
	if (plan->parameter_count) {
		zend_hash_clean(&plan->native_positions);
		for (size_t i = 0; i < plan->parameter_count; i++) {
			zval slot;
			ZVAL_LONG(&slot, (zend_long)i);
			zend_hash_index_add_new(&plan->native_positions, (zend_ulong)i, &slot);
		}
		for (size_t i = 0; i < plan->native_occurrence_count; i++) {
			const pdo_duckdb_sql_placeholder *placeholder = &plan->native_placeholders[i];
			if (placeholder->numeric &&
					!zend_hash_index_exists(&plan->native_positions, (zend_ulong)placeholder->index)) {
				zval slot;
				ZVAL_LONG(&slot, (zend_long)zend_hash_num_elements(&plan->native_positions));
				zend_hash_index_add_new(&plan->native_positions, (zend_ulong)placeholder->index, &slot);
			}
		}
	}
	plan->binding_count = zend_hash_num_elements(&plan->native_positions);
	return plan;
}

void pdo_duckdb_sql_plan_destroy(pdo_duckdb_sql_plan *plan)
{
	if (!plan) {
		return;
	}
	zend_string_release(plan->sql);
	zend_hash_destroy(&plan->names);
	zend_hash_destroy(&plan->native_names);
	zend_hash_destroy(&plan->native_positions);
	if (plan->placeholders) {
		efree(plan->placeholders);
	}
	if (plan->native_placeholders) {
		efree(plan->native_placeholders);
	}
	efree(plan);
}

size_t pdo_duckdb_sql_binding_slot(const pdo_duckdb_sql_plan *plan, zend_ulong position)
{
	if (position >= plan->binding_count) {
		return SIZE_MAX;
	}
	zval *slot = zend_hash_index_find(&plan->native_positions, position);
	return slot ? (size_t)Z_LVAL_P(slot) : SIZE_MAX;
}

static void pdo_duckdb_sql_append_bytes(smart_str *output, const char *data, size_t length, bool blob)
{
	if (blob || memchr(data, '\0', length)) {
		static const char hex[] = "0123456789abcdef";
		smart_str_appends(output, blob ? "system.main.from_hex('" : "system.main.decode(system.main.from_hex('");
		for (size_t i = 0; i < length; i++) {
			unsigned char c = (unsigned char)data[i];
			smart_str_appendc(output, hex[c >> 4]);
			smart_str_appendc(output, hex[c & 15]);
		}
		smart_str_appends(output, blob ? "')" : "'))");
	} else {
		smart_str_appendc(output, '\'');
		for (size_t i = 0; i < length; i++) {
			if (data[i] == '\'') {
				smart_str_appendc(output, '\'');
			}
			smart_str_appendc(output, data[i]);
		}
		smart_str_appendc(output, '\'');
	}
}

static zend_string *pdo_duckdb_sql_render(const pdo_duckdb_sql_plan *plan,
	const pdo_duckdb_sql_binding *values, bool probe)
{
	smart_str output = {0};
	size_t copied = 0;
	size_t pdo_index = 0, native_index = 0;
	size_t native_count = values ? plan->native_occurrence_count : 0;
	while (pdo_index < plan->occurrence_count || native_index < native_count) {
		bool native = native_index < native_count && (pdo_index == plan->occurrence_count ||
			plan->native_placeholders[native_index].offset < plan->placeholders[pdo_index].offset);
		const pdo_duckdb_sql_placeholder *placeholder = native
			? &plan->native_placeholders[native_index++] : &plan->placeholders[pdo_index++];
		size_t slot = placeholder->index;
		if (native) {
			slot = plan->parameter_count && !placeholder->numeric ? SIZE_MAX
				: pdo_duckdb_sql_binding_slot(plan, (zend_ulong)placeholder->index);
		}
		smart_str_appendl(&output, ZSTR_VAL(plan->sql) + copied, placeholder->offset - copied);
		if (native && (slot == SIZE_MAX || !values[slot].value)) {
			smart_str_appendl(&output, ZSTR_VAL(plan->sql) + placeholder->offset, placeholder->length);
		} else if (values) {
			/* Keep a negative number or quoted value separate from preceding SQL tokens. */
			smart_str_appendc(&output, ' ');
			if (values[slot].quote) {
				pdo_duckdb_sql_append_bytes(&output, ZSTR_VAL(values[slot].value),
					ZSTR_LEN(values[slot].value), values[slot].blob);
			} else {
				smart_str_append(&output, values[slot].value);
			}
			smart_str_appendc(&output, ' ');
		} else if (probe) {
			smart_str_appends(&output, " '' ");
		} else {
			smart_str_appendc(&output, '$');
			smart_str_append_long(&output, (zend_long)placeholder->index + 1);
		}
		copied = placeholder->offset + placeholder->length;
	}
	smart_str_appendl(&output, ZSTR_VAL(plan->sql) + copied, ZSTR_LEN(plan->sql) - copied);
	smart_str_0(&output);
	return output.s ? output.s : ZSTR_EMPTY_ALLOC();
}

zend_string *pdo_duckdb_sql_native(const pdo_duckdb_sql_plan *plan)
{
	return pdo_duckdb_sql_render(plan, NULL, false);
}

zend_string *pdo_duckdb_sql_parse_probe(const pdo_duckdb_sql_plan *plan)
{
	return pdo_duckdb_sql_render(plan, NULL, true);
}

zend_string *pdo_duckdb_sql_expand(const pdo_duckdb_sql_plan *plan,
	const pdo_duckdb_sql_binding *values, size_t count, zend_string **error)
{
	*error = NULL;
	if (count != plan->binding_count) {
		*error = zend_string_init("number of bound variables does not match number of tokens",
			sizeof("number of bound variables does not match number of tokens") - 1, 0);
		return NULL;
	}
	for (size_t i = 0; i < plan->parameter_count; i++) {
		if (!values || !values[i].value) {
			*error = zend_string_init("parameter was not defined", sizeof("parameter was not defined") - 1, 0);
			return NULL;
		}
	}
	return pdo_duckdb_sql_render(plan, values, false);
}

zend_string *pdo_duckdb_sql_quote_bytes(const char *data, size_t length, bool blob)
{
	smart_str output = {0};
	pdo_duckdb_sql_append_bytes(&output, data, length, blob);
	smart_str_0(&output);
	return output.s;
}
