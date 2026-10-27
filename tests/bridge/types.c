#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_variant.h"
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

static pdd_bridge_value *cell(duckdb_data_chunk chunk, idx_t column, idx_t row) {
    pdd_bridge_value *value = NULL;
    OK(pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(chunk, column), row,
                                     duckdb_data_chunk_get_size(chunk), &value, &error));
    REQUIRE(value != NULL);
    return value;
}

static void expect_bytes(pdd_bridge_bytes bytes, const char *expected, size_t length) {
    REQUIRE(bytes.data != NULL);
    if (bytes.length != length || memcmp(bytes.data, expected, length < bytes.length ? length : bytes.length) != 0) {
        fprintf(stderr, "Expected %zu bytes [%.*s], received %zu bytes [%.*s]\n", length, (int)length, expected,
                bytes.length, (int)bytes.length, bytes.data);
    }
    REQUIRE(bytes.length == length);
    REQUIRE(memcmp(bytes.data, expected, length) == 0);
    REQUIRE(bytes.data[length] == 0);
}

static void expect_render(pdd_bridge_value *value, const char *expected, size_t length) {
    pdd_bridge_bytes rendered = {0};
    bool is_null = true;
    OK(pdd_bridge_value_render(value, NULL, &rendered, &is_null, &error));
    REQUIRE(!is_null);
    expect_bytes(rendered, expected, length);
    pdd_bridge_bytes_free(&rendered);
    OK(pdd_bridge_value_render_pdo(value, NULL, &rendered, &is_null, &error));
    REQUIRE(!is_null);
    expect_bytes(rendered, expected, length);
    pdd_bridge_bytes_free(&rendered);
}

static pdd_bridge_variant *variant_node(pdd_bridge_value *value) {
    duckdb_value native = NULL;
    pdd_bridge_variant *out = NULL;
    OK(pdd_bridge_value_to_c(value, &native, &error));
    OK(pdd_bridge_variant_from_c(native, &out, &error));
    duckdb_destroy_value(&native);
    return out;
}

static pdd_bridge_value *variant_scalar(pdd_bridge_variant *node) {
    duckdb_value native = NULL;
    pdd_bridge_value *out = NULL;
    OK(pdd_bridge_variant_scalar(node, &native, &error));
    OK(pdd_bridge_value_from_c(native, &out, &error));
    duckdb_destroy_value(&native);
    return out;
}

static void differential(duckdb_connection connection) {
    const char *expressions[] = {
        "['a'||chr(0)||i::VARCHAR, 'b'' c', 'x,y']",
        "['a'||chr(0)||i::VARCHAR, 'b'' c']::VARCHAR[2]",
        "{'nul': 'x'||chr(0)||i::VARCHAR, 'num': i::DECIMAL(30,9), 'empty': []::INTEGER[]}",
        "(i::BIGINT, 'x'||chr(0)||i::VARCHAR)",
        "map([i,i+1], ['a'||chr(0)||'b', 'c'])",
        "union_value(word := 'a'||chr(0)||i::VARCHAR)",
        "{'nul': 'x'||chr(0)||i::VARCHAR, 'large': 123456789012345678901234567890::HUGEINT, 'null': NULL}::VARIANT",
        "[i::VARIANT, NULL::VARIANT, {'v': 'a'||chr(0)||'b'}::VARIANT]",
        "[(TIME '12:34:56.123456'), (TIME '01:23:45.654321')]::TIME[2]",
        "[(TIMESTAMP '2026-10-26 12:34:56.123456'), (TIMESTAMP '1969-12-31 23:59:59.123456')]::TIMESTAMP[2]"
    };
    for (size_t i = 0; i < sizeof(expressions) / sizeof(expressions[0]); i++) {
        char sql[2048];
        const char *pdo_oracle = i == 6 ?
            "CAST({'nul': 'x'||chr(0)||i::VARCHAR, 'large': 123456789012345678901234567890::HUGEINT, 'null': NULL} AS VARCHAR)" :
            "CAST(v AS VARCHAR)";
        int written = snprintf(sql, sizeof(sql),
            "SELECT v, CAST(v AS VARCHAR), %s FROM (SELECT i, %s AS v FROM range(4097) t(i))", pdo_oracle, expressions[i]);
        REQUIRE(written > 0 && (size_t)written < sizeof(sql));
        duckdb_result result;
        query(connection, sql, &result);
        idx_t checked = 0;
        duckdb_data_chunk chunk;
        while ((chunk = duckdb_fetch_chunk(result)) != NULL) {
            duckdb_vector reference = duckdb_data_chunk_get_vector(chunk, 1);
            duckdb_string_t *strings = duckdb_vector_get_data(reference);
            duckdb_string_t *pdo_strings = duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 2));
            for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk); row++) {
                pdd_bridge_value *value = cell(chunk, 0, row);
                pdd_bridge_bytes rendered = {0};
                bool is_null = true;
                OK(pdd_bridge_value_render(value, NULL, &rendered, &is_null, &error));
                REQUIRE(!is_null);
                expect_bytes(rendered, duckdb_string_t_data(&strings[row]), duckdb_string_t_length(strings[row]));
                pdd_bridge_bytes_free(&rendered);
                OK(pdd_bridge_value_render_pdo(value, NULL, &rendered, &is_null, &error));
                REQUIRE(!is_null);
                expect_bytes(rendered, duckdb_string_t_data(&pdo_strings[row]), duckdb_string_t_length(pdo_strings[row]));
                pdd_bridge_bytes_free(&rendered);
                pdd_bridge_value_destroy(&value);
                checked++;
            }
            duckdb_destroy_data_chunk(&chunk);
        }
        REQUIRE(checked == 4097);
        duckdb_destroy_result(&result);
    }
}

static void expect_column_name(duckdb_result *result, idx_t column, const char *expected, size_t length) {
    pdd_bridge_bytes name = {0};
    OK(pdd_bridge_c_result_column_name(result, column, &name, &error));
    expect_bytes(name, expected, length);
    pdd_bridge_bytes_free(&name);
}

static void column_names(duckdb_connection connection) {
    const char *nul_sql = "SELECT UNNEST(from_json('{\"a\":1,\"a\\u0000b\":2}', "
                          "'{\"a\":\"INTEGER\",\"a\\u0000b\":\"INTEGER\"}'))";
    duckdb_result result = {0};
    pdd_bridge_bytes name = {0};
    REQUIRE(pdd_bridge_c_result_column_name(NULL, 0, &name, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(name.data == NULL && name.length == 0 && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_c_result_column_name(&result, 0, &name, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(name.data == NULL && name.length == 0 && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(duckdb_query(connection, "SELECT missing_column", &result) == DuckDBError);
    REQUIRE(pdd_bridge_c_result_column_name(&result, 0, &name, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(name.data == NULL && name.length == 0 && error.data != NULL);
    pdd_bridge_error_free(&error);
    duckdb_destroy_result(&result);
    const bool old_json_keys = strcmp(duckdb_library_version(), "v1.5.3") == 0;
    if (old_json_keys) {
        REQUIRE(duckdb_query(connection, nul_sql, &result) == DuckDBError);
        REQUIRE(strstr(duckdb_result_error(&result), "Duplicate keys in object in JSON structure") != NULL);
        duckdb_destroy_result(&result);
    } else {
        query(connection, nul_sql, &result);
        REQUIRE(duckdb_column_count(&result) == 2);
        expect_column_name(&result, 0, "a", 1);
        expect_column_name(&result, 1, "a\0b", 3);
        duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
        REQUIRE(chunk != NULL && duckdb_data_chunk_get_size(chunk) == 1);
        REQUIRE(((int32_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)))[0] == 1);
        REQUIRE(((int32_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1)))[0] == 2);
        duckdb_destroy_data_chunk(&chunk);
        OK(pdd_bridge_c_result_column_name(&result, 1, &name, &error));
        duckdb_destroy_result(&result);
        expect_bytes(name, "a\0b", 3);
        pdd_bridge_bytes_free(&name);
    }
    char sql[512];
    int written = snprintf(sql, sizeof(sql), "%s WHERE false", old_json_keys ? "SELECT 1 AS a, 2 AS b" : nul_sql);
    REQUIRE(written > 0 && (size_t)written < sizeof(sql));
    query(connection, sql, &result);
    REQUIRE(duckdb_row_count(&result) == 0);
    expect_column_name(&result, 0, "a", 1);
    expect_column_name(&result, 1, old_json_keys ? "b" : "a\0b", old_json_keys ? 1 : 3);
    REQUIRE(pdd_bridge_c_result_column_name(&result, 2, &name, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(name.data == NULL && name.length == 0 && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_c_result_column_name(&result, 0, NULL, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(error.data != NULL);
    pdd_bridge_error_free(&error);
    duckdb_destroy_result(&result);
    REQUIRE(pdd_bridge_c_result_column_name(&result, 0, &name, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(name.data == NULL && name.length == 0 && error.data != NULL);
    pdd_bridge_error_free(&error);
    written = snprintf(sql, sizeof(sql), "%s FROM range(100000)", old_json_keys ? "SELECT 1 AS a, 2 AS b" : nul_sql);
    REQUIRE(written > 0 && (size_t)written < sizeof(sql));
    duckdb_prepared_statement prepared = NULL;
    duckdb_pending_result pending = NULL;
    REQUIRE(duckdb_prepare(connection, sql, &prepared) == DuckDBSuccess);
    REQUIRE(duckdb_pending_prepared_streaming(prepared, &pending) == DuckDBSuccess);
    REQUIRE(duckdb_execute_pending(pending, &result) == DuckDBSuccess);
    duckdb_destroy_pending(&pending);
    REQUIRE(duckdb_result_is_streaming(result));
    expect_column_name(&result, 0, "a", 1);
    expect_column_name(&result, 1, old_json_keys ? "b" : "a\0b", old_json_keys ? 1 : 3);
    idx_t rows = 0;
    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(result)) != NULL) {
        rows += duckdb_data_chunk_get_size(chunk);
        expect_column_name(&result, 1, old_json_keys ? "b" : "a\0b", old_json_keys ? 1 : 3);
        duckdb_destroy_data_chunk(&chunk);
    }
    REQUIRE(rows == 100000 && duckdb_result_error(&result) == NULL);
    expect_column_name(&result, 1, old_json_keys ? "b" : "a\0b", old_json_keys ? 1 : 3);
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&prepared);
}

static void metadata(duckdb_connection connection) {
    duckdb_result result;
    query(connection, "SELECT NULL::DECIMAL(38,12) AS d, {'name': [1::DECIMAL(20,3)]} AS s, (1, 'x') AS t, '[1]'::JSON AS j WHERE false", &result);
    const char *declarations[] = {"DECIMAL(38,12)", "STRUCT(\"name\" DECIMAL(20,3)[])", NULL, "JSON"};
    for (idx_t column = 0; column < 4; column++) {
        duckdb_logical_type type = duckdb_column_logical_type(&result, column);
        pdd_bridge_bytes declaration = {0};
        OK(pdd_bridge_type_declaration(type, &declaration, &error));
        if (declarations[column]) {
            expect_bytes(declaration, declarations[column], strlen(declarations[column]));
        } else {
            bool tuple = false;
            OK(pdd_bridge_type_is_tuple(type, &tuple, &error));
            REQUIRE(tuple);
            REQUIRE(strcmp(declaration.data, "TUPLE(INTEGER, VARCHAR)") == 0 ||
                    strcmp(declaration.data, "STRUCT(INTEGER, VARCHAR)") == 0);
        }
        if (column == 3) {
            pdd_bridge_bytes alias = {0};
            OK(pdd_bridge_type_alias(type, &alias, &error));
            expect_bytes(alias, "JSON", 4);
            pdd_bridge_bytes_free(&alias);
        }
        pdd_bridge_bytes_free(&declaration);
        duckdb_destroy_logical_type(&type);
    }
    duckdb_destroy_result(&result);
}

static void empty_key_variants(duckdb_connection connection) {
    const struct {
        const char *json;
        const char *rendered;
        size_t length;
    } cases[] = {
        {"{\"\":3,\"a\":[1,2]}", "{'': 3, 'a': [1, 2]}", sizeof("{'': 3, 'a': [1, 2]}") - 1},
        {"{\"outer\":{\"\":3,\"a\":[1,2]}}", "{'outer': {'': 3, 'a': [1, 2]}}", sizeof("{'outer': {'': 3, 'a': [1, 2]}}") - 1},
        {"{\"\":3,\"n\\u0000k\":\"a\\u0000b\"}", "{'': 3, 'n\0k': a\0b}", sizeof("{'': 3, 'n\0k': a\0b}") - 1},
        {"{\"a\":[1,2],\"\":3}", "{'a': [1, 2], '': 3}", sizeof("{'a': [1, 2], '': 3}") - 1},
        {"[{\"\":42},[1,\"x\"]]", "[{'': 42}, [1, x]]", sizeof("[{'': 42}, [1, x]]") - 1}
    };
    const char *containers[][2] = {
        {"v", "r"}, {"[v]", "[r]"}, {"[v]::VARIANT[1]", "[r]::VARIANT[1]"},
        {"{'v':v,'s':'a,b'||chr(0)||'c'}", "{'v':r,'s':'a,b'||chr(0)||'c'}"},
        {"(v, 12.30::DECIMAL(9,2))", "(r, 12.30::DECIMAL(9,2))"},
        {"map(['key'],[v])", "map(['key'],[r])"}, {"map([v],[42])", "map([r],[42])"},
        {"union_value(item := v)", "union_value(item := r)"},
        {"{'nested':v}::VARIANT", "{'nested':r}::VARIANT"},
        {"[v, 5::VARIANT]::VARIANT", "[r, 5::VARIANT]::VARIANT"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (size_t j = 0; j < sizeof(containers) / sizeof(containers[0]); j++) {
            char sql[2048];
            int written = snprintf(sql, sizeof(sql),
                "WITH x AS (SELECT '%s'::JSON::VARIANT AS v, 'RENDER_SENTINEL'::VARIANT AS r) SELECT %s, CAST(%s AS VARCHAR), %s FROM x",
                cases[i].json, containers[j][0], containers[j][1], i < 4 ? "CAST(v AS VARCHAR)" : "NULL::VARCHAR");
            REQUIRE(written > 0 && (size_t)written < sizeof(sql));
            duckdb_result result;
            query(connection, sql, &result);
            duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
            REQUIRE(chunk && duckdb_data_chunk_get_size(chunk) == 1);
            pdd_bridge_value *value = cell(chunk, 0, 0);
            if (j == 0 && i < 4) {
                pdd_bridge_bytes engine = {0};
                bool is_null = true;
                OK(pdd_bridge_value_render(value, NULL, &engine, &is_null, &error));
                REQUIRE(!is_null);
                duckdb_string_t *original = duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 2));
                expect_bytes(engine, duckdb_string_t_data(original), duckdb_string_t_length(*original));
                pdd_bridge_bytes_free(&engine);
            }
            duckdb_string_t *reference = duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1));
            const char *source = duckdb_string_t_data(reference);
            size_t source_length = duckdb_string_t_length(*reference);
            size_t position = 0;
            const size_t sentinel_length = sizeof("RENDER_SENTINEL") - 1;
            while (position + sentinel_length <= source_length &&
                   memcmp(source + position, "RENDER_SENTINEL", sentinel_length)) {
                position++;
            }
            REQUIRE(position + sentinel_length <= source_length);
            size_t expected_length = source_length - sentinel_length + cases[i].length;
            char *expected = malloc(expected_length);
            REQUIRE(expected != NULL);
            memcpy(expected, source, position);
            memcpy(expected + position, cases[i].rendered, cases[i].length);
            memcpy(expected + position + cases[i].length, source + position + sentinel_length,
                   source_length - position - sentinel_length);
            for (unsigned context = 0; context < 2; context++) {
                pdd_bridge_bytes rendered = {0};
                bool is_null = true;
                OK(pdd_bridge_value_render_pdo(value, context ? connection : NULL, &rendered, &is_null, &error));
                REQUIRE(!is_null);
                if (rendered.length != expected_length ||
                    memcmp(rendered.data, expected, rendered.length < expected_length ? rendered.length : expected_length)) {
                    fprintf(stderr, "VARIANT rendering differs: %s\n", sql);
                }
                expect_bytes(rendered, expected, expected_length);
                pdd_bridge_bytes_free(&rendered);
            }
            if (i == 0 && j == 0) {
                pdd_bridge_variant *object = variant_node(value), *child = NULL, *item = NULL;
                pdd_bridge_variant_kind kind;
                idx_t count = 0;
                pdd_bridge_bytes key = {0};
                OK(pdd_bridge_variant_info(object, &kind, &count, &error));
                REQUIRE(kind == PDD_BRIDGE_VARIANT_OBJECT && count == 2);
                OK(pdd_bridge_variant_key(object, 0, &key, &error));
                expect_bytes(key, "", 0);
                pdd_bridge_bytes_free(&key);
                OK(pdd_bridge_variant_child(object, 0, &child, &error));
                pdd_bridge_value *scalar = variant_scalar(child);
                expect_render(scalar, "3", 1);
                pdd_bridge_value_destroy(&scalar);
                pdd_bridge_variant_destroy(&child);
                OK(pdd_bridge_variant_child(object, 1, &child, &error));
                OK(pdd_bridge_variant_info(child, &kind, &count, &error));
                REQUIRE(kind == PDD_BRIDGE_VARIANT_ARRAY && count == 2);
                for (idx_t index = 0; index < count; index++) {
                    OK(pdd_bridge_variant_child(child, index, &item, &error));
                    scalar = variant_scalar(item);
                    expect_render(scalar, index ? "2" : "1", 1);
                    pdd_bridge_value_destroy(&scalar);
                    pdd_bridge_variant_destroy(&item);
                }
                pdd_bridge_variant_destroy(&child);
                pdd_bridge_variant_destroy(&object);
            }
            free(expected);
            pdd_bridge_value_destroy(&value);
            duckdb_destroy_data_chunk(&chunk);
            duckdb_destroy_result(&result);
        }
    }
    for (size_t j = 0; j < 8; j++) {
        char sql[1024];
        int written = snprintf(sql, sizeof(sql),
            "WITH x AS (SELECT ('a,b'||chr(0)||'c')::VARIANT AS v, 'a,b'||chr(0)||'c' AS r) SELECT %s, CAST(%s AS VARCHAR) FROM x",
            containers[j][0], j == 2 ? "[r]::VARCHAR[1]" : containers[j][1]);
        REQUIRE(written > 0 && (size_t)written < sizeof(sql));
        duckdb_result result;
        query(connection, sql, &result);
        duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
        REQUIRE(chunk && duckdb_data_chunk_get_size(chunk) == 1);
        pdd_bridge_value *value = cell(chunk, 0, 0);
        duckdb_string_t *reference = duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1));
        pdd_bridge_bytes rendered = {0};
        bool is_null = true;
        OK(pdd_bridge_value_render_pdo(value, NULL, &rendered, &is_null, &error));
        REQUIRE(!is_null);
        expect_bytes(rendered, duckdb_string_t_data(reference), duckdb_string_t_length(*reference));
        pdd_bridge_bytes_free(&rendered);
        pdd_bridge_value_destroy(&value);
        duckdb_destroy_data_chunk(&chunk);
        duckdb_destroy_result(&result);
    }
}

static void variants_and_children(duckdb_connection connection) {
    duckdb_result result;
    query(connection, "SELECT {'d': 123456789012345678901234.123456789::DECIMAL(38,9), 'n': NULL, 'a': [1,2]}::VARIANT, map([42],[from_hex('00ff')]), union_value(word := 'a'||chr(0)||'b'), NULL::VARIANT", &result);
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    pdd_bridge_value *variant = cell(chunk, 0, 0), *child = NULL;
    pdd_bridge_variant *object = variant_node(variant);
    idx_t count = 0;
    pdd_bridge_variant_kind kind;
    OK(pdd_bridge_variant_info(object, &kind, &count, &error));
    REQUIRE(count == 3 && kind == PDD_BRIDGE_VARIANT_OBJECT);
    bool is_null = false;
    bool saw_decimal = false, saw_null = false, saw_array = false;
    for (idx_t index = 0; index < count; index++) {
        pdd_bridge_bytes field = {0};
        OK(pdd_bridge_variant_key(object, index, &field, &error));
        REQUIRE(field.length == 1);
        pdd_bridge_variant *node = NULL;
        OK(pdd_bridge_variant_child(object, index, &node, &error));
        if (field.data[0] == 'd') {
            child = variant_scalar(node);
            const char exact[] = "123456789012345678901234.123456789";
            expect_render(child, exact, sizeof(exact) - 1);
            saw_decimal = true;
        } else if (field.data[0] == 'n') {
            child = variant_scalar(node);
            OK(pdd_bridge_value_is_null(child, &is_null, &error));
            REQUIRE(is_null);
            saw_null = true;
        } else {
            REQUIRE(field.data[0] == 'a');
            idx_t array_size = 0;
            OK(pdd_bridge_variant_info(node, &kind, &array_size, &error));
            REQUIRE(kind == PDD_BRIDGE_VARIANT_ARRAY && array_size == 2);
            for (idx_t element = 0; element < array_size; element++) {
                pdd_bridge_variant *item = NULL;
                OK(pdd_bridge_variant_child(node, element, &item, &error));
                child = variant_scalar(item);
                expect_render(child, element == 0 ? "1" : "2", 1);
                pdd_bridge_value_destroy(&child);
                pdd_bridge_variant_destroy(&item);
            }
            saw_array = true;
        }
        pdd_bridge_bytes_free(&field);
        pdd_bridge_value_destroy(&child);
        pdd_bridge_variant_destroy(&node);
    }
    REQUIRE(saw_decimal && saw_null && saw_array);
    pdd_bridge_variant_destroy(&object);
    pdd_bridge_value_destroy(&variant);

    pdd_bridge_value *map = cell(chunk, 1, 0), *entry = NULL, *key = NULL, *value = NULL;
    OK(pdd_bridge_value_child_count(map, &count, &error));
    REQUIRE(count == 1);
    OK(pdd_bridge_value_child(map, 0, &entry, &error));
    OK(pdd_bridge_value_child(entry, 0, &key, &error));
    expect_render(key, "42", 2);
    OK(pdd_bridge_value_child(entry, 1, &value, &error));
    pdd_bridge_bytes bytes = {0};
    OK(pdd_bridge_value_string(value, &bytes, &error));
    expect_bytes(bytes, "\0\xff", 2);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&map);
    pdd_bridge_value_destroy(&entry);
    pdd_bridge_value_destroy(&key);
    pdd_bridge_value_destroy(&value);

    pdd_bridge_value *tagged = cell(chunk, 2, 0);
    OK(pdd_bridge_value_child_count(tagged, &count, &error));
    REQUIRE(count == 2);
    OK(pdd_bridge_value_child(tagged, 0, &child, &error));
    expect_render(child, "0", 1);
    pdd_bridge_value_destroy(&child);
    OK(pdd_bridge_value_child(tagged, 1, &child, &error));
    OK(pdd_bridge_value_string(child, &bytes, &error));
    expect_bytes(bytes, "a\0b", 3);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&child);
    duckdb_logical_type type = NULL;
    OK(pdd_bridge_value_type(tagged, &type, &error));
    OK(pdd_bridge_type_field_name(type, 0, &bytes, &error));
    expect_bytes(bytes, "word", 4);
    pdd_bridge_bytes_free(&bytes);
    duckdb_destroy_logical_type(&type);
    pdd_bridge_value_destroy(&tagged);

    variant = cell(chunk, 3, 0);
    OK(pdd_bridge_value_render(variant, NULL, &bytes, &is_null, &error));
    REQUIRE(is_null && bytes.data == NULL && bytes.length == 0);
    object = variant_node(variant);
    OK(pdd_bridge_variant_info(object, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_SQL_NULL && count == 0);
    child = variant_scalar(object);
    OK(pdd_bridge_value_is_null(child, &is_null, &error));
    REQUIRE(is_null);
    pdd_bridge_value_destroy(&child);
    pdd_bridge_variant_destroy(&object);
    pdd_bridge_value_destroy(&variant);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
}

static void construction(void) {
    duckdb_value scalar = duckdb_create_varchar_length("a\0b", 3);
    pdd_bridge_value *value = NULL, *structure = NULL, *variant = NULL, *decoded = NULL;
    OK(pdd_bridge_value_from_c(scalar, &value, &error));
    duckdb_destroy_value(&scalar);
    pdd_bridge_bytes name = {(char *)"k\0y", 3};
    OK(pdd_bridge_value_struct(&name, &value, 1, false, &structure, &error));
    duckdb_logical_type type = NULL;
    OK(pdd_bridge_value_type(structure, &type, &error));
    pdd_bridge_bytes bytes = {0};
    OK(pdd_bridge_type_field_name(type, 0, &bytes, &error));
    expect_bytes(bytes, name.data, name.length);
    pdd_bridge_bytes_free(&bytes);
    duckdb_destroy_logical_type(&type);
    type = duckdb_create_logical_type(DUCKDB_TYPE_VARIANT);
    REQUIRE(pdd_bridge_value_cast(structure, type, NULL, &variant, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(variant == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    duckdb_logical_type incomplete_list = duckdb_create_list_type(type);
    REQUIRE(pdd_bridge_value_list(incomplete_list, NULL, 0, &decoded, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(decoded == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_type_declaration(incomplete_list, &bytes, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(bytes.data == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    duckdb_destroy_logical_type(&incomplete_list);
    duckdb_destroy_logical_type(&type);
    OK(pdd_bridge_type_variant(&type, &error));
    OK(pdd_bridge_value_cast(structure, type, NULL, &variant, &error));
    pdd_bridge_variant *object = variant_node(variant), *node = NULL;
    pdd_bridge_variant_kind kind;
    idx_t count = 0;
    OK(pdd_bridge_variant_info(object, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_OBJECT && count == 1);
    OK(pdd_bridge_variant_key(object, 0, &bytes, &error));
    expect_bytes(bytes, name.data, name.length);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value *child = NULL;
    OK(pdd_bridge_variant_child(object, 0, &node, &error));
    child = variant_scalar(node);
    OK(pdd_bridge_value_string(child, &bytes, &error));
    expect_bytes(bytes, "a\0b", 3);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&child);
    pdd_bridge_variant_destroy(&node);
    pdd_bridge_variant_destroy(&object);
    pdd_bridge_value_destroy(&variant);
    pdd_bridge_value_destroy(&structure);

    scalar = duckdb_create_varchar("{\"json\":true}");
    pdd_bridge_value *json_text = NULL;
    OK(pdd_bridge_value_from_c(scalar, &json_text, &error));
    duckdb_destroy_value(&scalar);
    OK(pdd_bridge_value_cast(json_text, type, NULL, &variant, &error));
    node = variant_node(variant);
    decoded = variant_scalar(node);
    OK(pdd_bridge_value_string(decoded, &bytes, &error));
    expect_bytes(bytes, "{\"json\":true}", 13);
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&decoded);
    pdd_bridge_variant_destroy(&node);
    pdd_bridge_value_destroy(&variant);
    pdd_bridge_value_destroy(&json_text);
    duckdb_destroy_logical_type(&type);

    OK(pdd_bridge_value_struct(NULL, &value, 1, true, &structure, &error));
    OK(pdd_bridge_value_type(structure, &type, &error));
    bool tuple = false;
    OK(pdd_bridge_type_is_tuple(type, &tuple, &error));
    REQUIRE(tuple);
    duckdb_destroy_logical_type(&type);
    pdd_bridge_value_destroy(&structure);
    type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    OK(pdd_bridge_value_list(type, NULL, 0, &structure, &error));
    expect_render(structure, "[]", 2);
    pdd_bridge_value_destroy(&structure);
    duckdb_destroy_logical_type(&type);
    pdd_bridge_bytes empty_name = {(char *)"", 0};
    REQUIRE(pdd_bridge_value_struct(&empty_name, &value, 1, false, &structure, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(structure == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    OK(pdd_bridge_value_struct(NULL, NULL, 0, false, &structure, &error));
    expect_render(structure, "{}", 2);
    pdd_bridge_value_destroy(&structure);

    type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    REQUIRE(pdd_bridge_value_cast(value, type, NULL, &decoded, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(decoded == NULL && error.data != NULL && error.length > 0);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_value_child(value, 1, &decoded, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(decoded == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    REQUIRE(pdd_bridge_type_field_name(type, 0, &bytes, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(bytes.data == NULL && error.data != NULL);
    pdd_bridge_error_free(&error);
    duckdb_destroy_logical_type(&type);
    pdd_bridge_value_destroy(&value);
}

int main(void) {
    if (pdd_bridge_check(&error) != PDD_BRIDGE_OK) {
        fprintf(stderr, "%.*s\n", (int)error.length, error.data ? error.data : "");
        pdd_bridge_error_free(&error);
        return 2;
    }
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    REQUIRE(duckdb_connect(database, &connection) == DuckDBSuccess);
    column_names(connection);
    metadata(connection);
    differential(connection);
    empty_key_variants(connection);
    variants_and_children(connection);
    construction();
    duckdb_disconnect(&connection);
    duckdb_close(&database);
    puts("bridge types: 40970 differential cells, metadata, typed VARIANT, NUL input, exact decimal, and error contracts passed");
    return 0;
}
