#ifndef PDO_DUCKDB_VALUE_H
#define PDO_DUCKDB_VALUE_H

#include "php.h"
#include <duckdb.h>

zend_result pdo_duckdb_value_is_structured(duckdb_logical_type type, bool *out, zend_string **error);
zend_result pdo_duckdb_value_native(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error);
zend_result pdo_duckdb_value_native_column(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error);
zend_result pdo_duckdb_value_render(duckdb_vector vector, idx_t row, idx_t row_count,
		zval *out, zend_string **error);

#endif
