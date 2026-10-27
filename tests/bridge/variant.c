#include "pdo_duckdb_bridge_variant.h"
#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pdd_bridge_error error;

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "Failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

#define OK(expression) do { \
    if ((expression) != PDD_BRIDGE_OK) { \
        fprintf(stderr, "Failed at %s:%d: %s: %.*s\n", __FILE__, __LINE__, #expression, \
                (int)error.length, error.data ? error.data : ""); \
        exit(1); \
    } \
} while (0)

static void query(duckdb_connection connection, const char *sql, duckdb_result *out) {
    if (duckdb_query(connection, sql, out) != DuckDBSuccess) {
        fprintf(stderr, "Query failed: %s\n%s\n", sql, duckdb_result_error(out));
        exit(1);
    }
}

static void bytes_equal(const char *actual, size_t actual_length, const char *expected, size_t expected_length) {
    REQUIRE(actual != NULL);
    REQUIRE(actual_length == expected_length);
    REQUIRE(memcmp(actual, expected, expected_length) == 0);
}

static void sql_string(duckdb_data_chunk chunk, idx_t column, const char *expected) {
    duckdb_vector vector = duckdb_data_chunk_get_vector(chunk, column);
    duckdb_string_t *strings = duckdb_vector_get_data(vector);
    bytes_equal(duckdb_string_t_data(&strings[0]), duckdb_string_t_length(strings[0]), expected, strlen(expected));
}

static pdd_bridge_variant *cell(duckdb_data_chunk chunk, idx_t column) {
    pdd_bridge_variant *out = NULL;
    OK(pdd_bridge_variant_from_vector(duckdb_data_chunk_get_vector(chunk, column), 0,
                                       duckdb_data_chunk_get_size(chunk), &out, &error));
    REQUIRE(out != NULL);
    return out;
}

static void info(pdd_bridge_variant *value, pdd_bridge_variant_kind expected_kind, idx_t expected_count) {
    pdd_bridge_variant_kind kind;
    idx_t count;
    OK(pdd_bridge_variant_info(value, &kind, &count, &error));
    if (kind != expected_kind || count != expected_count) {
        fprintf(stderr, "Expected kind %d count %llu, received kind %d count %llu\n", (int)expected_kind,
                (unsigned long long)expected_count, (int)kind, (unsigned long long)count);
    }
    REQUIRE(kind == expected_kind);
    REQUIRE(count == expected_count);
}

static pdd_bridge_variant *child(pdd_bridge_variant *value, idx_t index) {
    pdd_bridge_variant *out = NULL;
    OK(pdd_bridge_variant_child(value, index, &out, &error));
    REQUIRE(out != NULL);
    return out;
}

static pdd_bridge_variant *field(pdd_bridge_variant *value, const char *name, size_t length) {
    pdd_bridge_variant_kind kind;
    idx_t count;
    OK(pdd_bridge_variant_info(value, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_OBJECT);
    for (idx_t index = 0; index < count; index++) {
        pdd_bridge_bytes key = {0};
        OK(pdd_bridge_variant_key(value, index, &key, &error));
        bool matches = key.length == length && memcmp(key.data, name, length) == 0;
        pdd_bridge_bytes_free(&key);
        if (matches) {
            return child(value, index);
        }
    }
    REQUIRE(!"Missing expected object field");
    return NULL;
}

static void scalar_text(pdd_bridge_variant *value, const char *expected, size_t length) {
    info(value, PDD_BRIDGE_VARIANT_SCALAR, 0);
    duckdb_value scalar = NULL;
    pdd_bridge_value *wrapped = NULL;
    pdd_bridge_bytes rendered = {0};
    bool is_null = true;
    OK(pdd_bridge_variant_scalar(value, &scalar, &error));
    REQUIRE(scalar != NULL && !duckdb_is_null_value(scalar));
    OK(pdd_bridge_value_from_c(scalar, &wrapped, &error));
    duckdb_destroy_value(&scalar);
    OK(pdd_bridge_value_render(wrapped, NULL, &rendered, &is_null, &error));
    REQUIRE(!is_null);
    bytes_equal(rendered.data, rendered.length, expected, length);
    pdd_bridge_bytes_free(&rendered);
    pdd_bridge_value_destroy(&wrapped);
}

static void heterogeneous_object(duckdb_connection connection) {
    duckdb_result result;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    query(connection,
          "SELECT variant_typeof(v[1]), to_json(v), v "
          "FROM (SELECT '[{\"\":42},[1,\"x\"]]'::JSON::VARIANT AS v)", &result);
#else
    /* DuckDB 1.5 to_json uses the same lossy intermediate value conversion under test. */
    query(connection,
          "SELECT variant_typeof(v[1]), v "
          "FROM (SELECT '[{\"\":42},[1,\"x\"]]'::JSON::VARIANT AS v)", &result);
#endif
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    sql_string(chunk, 0, "OBJECT()");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    sql_string(chunk, 1, "[{\"\":42},[1,\"x\"]]");
    pdd_bridge_variant *root = cell(chunk, 2);
#else
    pdd_bridge_variant *root = cell(chunk, 1);
#endif
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    info(root, PDD_BRIDGE_VARIANT_ARRAY, 2);
    pdd_bridge_variant *object = child(root, 0), *array = child(root, 1);
    pdd_bridge_variant_destroy(&root);
    info(object, PDD_BRIDGE_VARIANT_OBJECT, 1);
    pdd_bridge_variant *number = field(object, "", 0);
    pdd_bridge_variant_destroy(&object);
    scalar_text(number, "42", 2);
    pdd_bridge_variant_destroy(&number);
    info(array, PDD_BRIDGE_VARIANT_ARRAY, 2);
    number = child(array, 0);
    pdd_bridge_variant *text = child(array, 1);
    pdd_bridge_variant_destroy(&array);
    scalar_text(number, "1", 1);
    scalar_text(text, "x", 1);
    pdd_bridge_variant_destroy(&number);
    pdd_bridge_variant_destroy(&text);
}

static void nulls_and_empty(duckdb_connection connection) {
    duckdb_result result;
    query(connection, "SELECT '[{},[],null]'::JSON::VARIANT, NULL::VARIANT, 'null'::JSON::VARIANT", &result);
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    pdd_bridge_variant *root = cell(chunk, 0), *sql_null = cell(chunk, 1), *json_null = cell(chunk, 2);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    info(root, PDD_BRIDGE_VARIANT_ARRAY, 3);
    pdd_bridge_variant *object = child(root, 0), *array = child(root, 1), *nested_null = child(root, 2);
    info(object, PDD_BRIDGE_VARIANT_OBJECT, 0);
    info(array, PDD_BRIDGE_VARIANT_ARRAY, 0);
    info(nested_null, PDD_BRIDGE_VARIANT_NULL, 0);
    info(sql_null, PDD_BRIDGE_VARIANT_SQL_NULL, 0);
    info(json_null, PDD_BRIDGE_VARIANT_SQL_NULL, 0);
    duckdb_value scalar = NULL;
    OK(pdd_bridge_variant_scalar(nested_null, &scalar, &error));
    REQUIRE(scalar != NULL && duckdb_is_null_value(scalar));
    duckdb_destroy_value(&scalar);
    OK(pdd_bridge_variant_scalar(sql_null, &scalar, &error));
    REQUIRE(scalar != NULL && duckdb_is_null_value(scalar));
    duckdb_destroy_value(&scalar);

    REQUIRE(pdd_bridge_variant_scalar(object, &scalar, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(scalar == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    pdd_bridge_variant *missing = NULL;
    REQUIRE(pdd_bridge_variant_child(array, 0, &missing, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(missing == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    pdd_bridge_bytes invalid_key = {0};
    REQUIRE(pdd_bridge_variant_key(root, 0, &invalid_key, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(invalid_key.data == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    pdd_bridge_variant_destroy(&root);
    pdd_bridge_variant_destroy(&object);
    pdd_bridge_variant_destroy(&array);
    pdd_bridge_variant_destroy(&nested_null);
    pdd_bridge_variant_destroy(&sql_null);
    pdd_bridge_variant_destroy(&json_null);
}

static void exact_scalars_and_names(duckdb_connection connection) {
    duckdb_result result;
    query(connection,
          "SELECT json_object('k'||chr(0)||'y', 'a'||chr(0)||'b')::VARIANT, "
          "{'d': 123456789012345678901234.123456789::DECIMAL(38,9), "
          "'i': 170141183460469231731687303715884105727::HUGEINT, "
          "'u': 340282366920938463463374607431768211455::UHUGEINT, "
          "'b': from_hex('00ff78'), 't': TIMESTAMP_NS '2026-10-26 12:34:56.123456789'}::VARIANT", &result);
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    pdd_bridge_variant *object = cell(chunk, 0), *scalars = cell(chunk, 1);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    info(object, PDD_BRIDGE_VARIANT_OBJECT, 1);
    pdd_bridge_variant *text = field(object, "k\0y", 3);
    scalar_text(text, "a\0b", 3);
    pdd_bridge_variant_destroy(&text);
    pdd_bridge_variant_destroy(&object);

    const char *names[] = {"d", "i", "u", "t"};
    const char *values[] = {"123456789012345678901234.123456789", "170141183460469231731687303715884105727",
                            "340282366920938463463374607431768211455", "2026-10-26 12:34:56.123456789"};
    duckdb_type types[] = {DUCKDB_TYPE_DECIMAL, DUCKDB_TYPE_HUGEINT, DUCKDB_TYPE_UHUGEINT, DUCKDB_TYPE_TIMESTAMP_NS};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        pdd_bridge_variant *value = field(scalars, names[i], 1);
        scalar_text(value, values[i], strlen(values[i]));
        duckdb_value scalar = NULL;
        OK(pdd_bridge_variant_scalar(value, &scalar, &error));
        duckdb_logical_type type = duckdb_get_value_type(scalar);
        REQUIRE(duckdb_get_type_id(type) == types[i]);
        if (types[i] == DUCKDB_TYPE_DECIMAL) {
            REQUIRE(duckdb_decimal_width(type) == 38 && duckdb_decimal_scale(type) == 9);
        }
        duckdb_destroy_value(&scalar);
        pdd_bridge_variant_destroy(&value);
    }
    pdd_bridge_variant *blob = field(scalars, "b", 1);
    duckdb_value scalar = NULL;
    pdd_bridge_value *wrapped = NULL;
    pdd_bridge_bytes bytes = {0};
    OK(pdd_bridge_variant_scalar(blob, &scalar, &error));
    OK(pdd_bridge_value_from_c(scalar, &wrapped, &error));
    duckdb_destroy_value(&scalar);
    OK(pdd_bridge_value_string(wrapped, &bytes, &error));
    bytes_equal(bytes.data, bytes.length, "\0\xffx", 3);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&wrapped);
    pdd_bridge_variant_destroy(&blob);
    pdd_bridge_variant_destroy(&scalars);
}

static void construction(duckdb_connection connection) {
    duckdb_result decimal_result;
    query(connection, "SELECT 123456789012345678901234.123456789::DECIMAL(38,9), NULL::BIGINT", &decimal_result);
    duckdb_data_chunk decimal_chunk = duckdb_fetch_chunk(decimal_result);
    pdd_bridge_value *decimal = NULL;
    duckdb_value decimal_value = NULL;
    duckdb_value typed_null = NULL;
    OK(pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(decimal_chunk, 0), 0, 1, &decimal, &error));
    OK(pdd_bridge_value_to_c(decimal, &decimal_value, &error));
    pdd_bridge_value_destroy(&decimal);
    OK(pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(decimal_chunk, 1), 0, 1, &decimal, &error));
    OK(pdd_bridge_value_to_c(decimal, &typed_null, &error));
    pdd_bridge_value_destroy(&decimal);
    duckdb_destroy_data_chunk(&decimal_chunk);
    duckdb_destroy_result(&decimal_result);

    duckdb_value values[] = {duckdb_create_int32(42), duckdb_create_varchar_length("a\0b", 3), decimal_value,
                             duckdb_create_varchar("{\"json\":true}"), duckdb_create_blob((const uint8_t *)"\0\xffx", 3)};
    pdd_bridge_bytes keys[] = {{(char *)"", 0}, {(char *)"n\0k", 3}, {(char *)"d", 1},
                               {(char *)"s", 1}, {(char *)"b", 1}};
    duckdb_value object = NULL, array = NULL;
    OK(pdd_bridge_variant_object(keys, values, 5, &object, &error));
    duckdb_value array_items[] = {object, values[3], values[2], typed_null};
    OK(pdd_bridge_variant_array(array_items, 4, &array, &error));
    duckdb_destroy_value(&typed_null);
    pdd_bridge_variant *root = NULL;
    OK(pdd_bridge_variant_from_c(array, &root, &error));
    duckdb_destroy_value(&array);
    info(root, PDD_BRIDGE_VARIANT_ARRAY, 4);
    pdd_bridge_variant *nested_object = child(root, 0);
    info(nested_object, PDD_BRIDGE_VARIANT_OBJECT, 5);
    pdd_bridge_variant *value = field(nested_object, "", 0);
    scalar_text(value, "42", 2);
    pdd_bridge_variant_destroy(&value);
    value = field(nested_object, "n\0k", 3);
    scalar_text(value, "a\0b", 3);
    pdd_bridge_variant_destroy(&value);
    value = field(nested_object, "d", 1);
    const char exact[] = "123456789012345678901234.123456789";
    scalar_text(value, exact, sizeof(exact) - 1);
    pdd_bridge_variant_destroy(&value);
    value = child(root, 1);
    scalar_text(value, "{\"json\":true}", 13);
    pdd_bridge_variant_destroy(&value);
    value = child(root, 3);
    info(value, PDD_BRIDGE_VARIANT_NULL, 0);
    pdd_bridge_variant_destroy(&value);
    pdd_bridge_variant_destroy(&nested_object);
    pdd_bridge_variant_destroy(&root);

    duckdb_prepared_statement statement = NULL;
    REQUIRE(duckdb_prepare(connection,
        "SELECT variant_typeof($1) LIKE 'OBJECT%', ($1[''])::INTEGER, "
        "hex(($1['n'||chr(0)||'k'])::VARCHAR), ($1['d'])::DECIMAL(38,9), ($1['s'])::VARCHAR, $1", &statement) == DuckDBSuccess);
    REQUIRE(duckdb_bind_value(statement, 1, object) == DuckDBSuccess);
    duckdb_result result;
    REQUIRE(duckdb_execute_prepared(statement, &result) == DuckDBSuccess);
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    REQUIRE(*(bool *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)));
    REQUIRE(*(int32_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1)) == 42);
    sql_string(chunk, 2, "610062");
    sql_string(chunk, 4, "{\"json\":true}");
    root = cell(chunk, 5);
    value = field(root, "d", 1);
    scalar_text(value, exact, sizeof(exact) - 1);
    pdd_bridge_variant_destroy(&value);
    pdd_bridge_variant_destroy(&root);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&statement);
    duckdb_destroy_value(&object);

    OK(pdd_bridge_variant_object(NULL, NULL, 0, &object, &error));
    OK(pdd_bridge_variant_from_c(object, &root, &error));
    info(root, PDD_BRIDGE_VARIANT_OBJECT, 0);
    pdd_bridge_variant_destroy(&root);
    duckdb_destroy_value(&object);
    OK(pdd_bridge_variant_array(NULL, 0, &array, &error));
    OK(pdd_bridge_variant_from_c(array, &root, &error));
    info(root, PDD_BRIDGE_VARIANT_ARRAY, 0);
    pdd_bridge_variant_destroy(&root);
    duckdb_destroy_value(&array);

    keys[1] = keys[0];
    REQUIRE(pdd_bridge_variant_object(keys, values, 2, &object, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(object == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_variant_array(values, (idx_t)UINT32_MAX + 1, &array, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(array == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_variant_object(keys, values, (idx_t)UINT32_MAX + 1, &object, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(object == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        duckdb_destroy_value(&values[index]);
    }
}

int main(void) {
    OK(pdd_bridge_check(&error));
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    REQUIRE(duckdb_connect(database, &connection) == DuckDBSuccess);
    heterogeneous_object(connection);
    nulls_and_empty(connection);
    exact_scalars_and_names(connection);
    construction(connection);
    duckdb_disconnect(&connection);
    duckdb_close(&database);
    puts("PASS: lossless VARIANT nodes, empty keys/containers, SQL and nested null, exact scalars, NUL bytes, and owned lifetime");
    return 0;
}
