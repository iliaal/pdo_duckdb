#ifndef PDO_DUCKDB_SQL_H
#define PDO_DUCKDB_SQL_H

#include "php.h"

typedef enum {
	PDO_DUCKDB_SQL_PARAMS_NONE,
	PDO_DUCKDB_SQL_PARAMS_POSITIONAL,
	PDO_DUCKDB_SQL_PARAMS_NAMED
} pdo_duckdb_sql_param_style;

typedef struct {
	size_t offset;
	size_t length;
	size_t index;
	bool numeric;
} pdo_duckdb_sql_placeholder;

typedef struct {
	zend_string *value;
	bool quote;
	bool blob;
} pdo_duckdb_sql_binding;

typedef struct {
	zend_string *sql;
	HashTable names;
	HashTable native_names;
	HashTable native_positions;
	pdo_duckdb_sql_placeholder *placeholders;
	pdo_duckdb_sql_placeholder *native_placeholders;
	size_t occurrence_count;
	size_t occurrence_capacity;
	size_t parameter_count;
	size_t binding_count;
	size_t native_occurrence_count;
	size_t native_occurrence_capacity;
	size_t native_parameter_count;
	size_t native_next_position;
	pdo_duckdb_sql_param_style style;
	bool legacy_parser;
	bool has_dollar_parameters;
	bool has_named_dollar_parameters;
	bool is_attach;
} pdo_duckdb_sql_plan;

/* The plan and its strings are request-owned; errors are caller-owned strings. */
pdo_duckdb_sql_plan *pdo_duckdb_sql_plan_create(zend_string *sql, bool legacy_parser, zend_string **error);
void pdo_duckdb_sql_plan_destroy(pdo_duckdb_sql_plan *plan);
zend_string *pdo_duckdb_sql_native(const pdo_duckdb_sql_plan *plan);
zend_string *pdo_duckdb_sql_parse_probe(const pdo_duckdb_sql_plan *plan);
size_t pdo_duckdb_sql_binding_slot(const pdo_duckdb_sql_plan *plan, zend_ulong position);
zend_string *pdo_duckdb_sql_expand(const pdo_duckdb_sql_plan *plan,
	const pdo_duckdb_sql_binding *values, size_t count, zend_string **error);
zend_string *pdo_duckdb_sql_quote_bytes(const char *data, size_t length, bool blob);

#endif
