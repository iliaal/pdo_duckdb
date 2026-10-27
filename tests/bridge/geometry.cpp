#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_variant.h"
#include "pdo_duckdb_bridge_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static pdd_bridge_error error;

#define REQUIRE(expression) do { \
    if (!(expression)) { std::fprintf(stderr, "Failed at %d: %s\n", __LINE__, #expression); std::exit(1); } \
} while (0)
#define OK(expression) do { \
    if ((expression) != PDD_BRIDGE_OK) { \
        std::fprintf(stderr, "Failed at %d: %s: %.*s\n", __LINE__, #expression, \
                     static_cast<int>(error.length), error.data ? error.data : ""); std::exit(1); \
    } \
} while (0)

int main() {
    OK(pdd_bridge_check(&error));
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
    REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
    const unsigned char wkb[] = {1,1,0,0,0,0,0,0,0,0,0,240,63,0,0,0,0,0,0,0,64};
    const char hex[] = "0101000000000000000000F03F0000000000000040";
    auto geometry = reinterpret_cast<duckdb_value>(new duckdb::Value(duckdb::Value::GEOMETRY(wkb, sizeof(wkb))));
    pdd_bridge_value *wrapped = nullptr;
    OK(pdd_bridge_value_from_c(geometry, &wrapped, &error));
    pdd_bridge_bytes bytes {};
    OK(pdd_bridge_value_string(wrapped, &bytes, &error));
    REQUIRE(bytes.length == sizeof(wkb) && !std::memcmp(bytes.data, wkb, sizeof(wkb)));
    pdd_bridge_bytes_free(&bytes);
    pdd_bridge_value_destroy(&wrapped);

    const char *pairs[][2] = {
        {"g", "h"},
        {"[g, NULL::GEOMETRY]", "[h, NULL::VARCHAR]"},
        {"[g]::GEOMETRY[1]", "[h]::VARCHAR[1]"},
        {"{'g':g, 's':'a'||chr(0)||'b'}", "{'g':h, 's':'a'||chr(0)||'b'}"},
        {"(g, 'a'||chr(0)||'b')", "(h, 'a'||chr(0)||'b')"},
        {"union_value(point := g)", "union_value(point := h)"},
        {"map([g], ['a'||chr(0)||'b'])", "map([h], ['a'||chr(0)||'b'])"},
        {"map(['x'], [[g]])", "map(['x'], [[h]])"},
        {"map([g::VARIANT,h::VARIANT],[1,2])",
         "replace(CAST(map([h::VARIANT,'OTHER_KEY'::VARIANT],[1,2]) AS VARCHAR), 'OTHER_KEY', h)"},
        {"g::VARIANT", "h::VARIANT"},
        {"[g::VARIANT, 3::VARIANT, '{\"n\":null}'::JSON::VARIANT]::VARIANT",
         "[h::VARIANT, 3::VARIANT, '{\"n\":null}'::JSON::VARIANT]::VARIANT"},
        {"struct_concat(from_json('{\"n\\u0000k\":\"a\\u0000b\"}', '{\"n\\u0000k\":\"VARCHAR\"}'), {'g':g})",
         "struct_concat(from_json('{\"n\\u0000k\":\"a\\u0000b\"}', '{\"n\\u0000k\":\"VARCHAR\"}'), {'g':h})"},
        {"{'nested':'{\"n\":3}'::JSON::VARIANT, 'g':g}::VARIANT",
         "{'nested':'{\"n\":3}'::JSON::VARIANT, 'g':h}"}
    };
    for (auto &pair : pairs) {
        auto sql = duckdb::string("WITH x AS (SELECT $1 AS g, $2 AS h) SELECT ") + pair[0] +
                   ", CAST(" + pair[1] + " AS VARCHAR) FROM x";
        duckdb_prepared_statement prepared = nullptr;
        if (duckdb_prepare(con, sql.c_str(), &prepared) != DuckDBSuccess) {
            std::fprintf(stderr, "%s\n%s\n", sql.c_str(), duckdb_prepare_error(prepared));
            return 1;
        }
        REQUIRE(duckdb_bind_value(prepared, 1, geometry) == DuckDBSuccess);
        REQUIRE(duckdb_bind_varchar(prepared, 2, hex) == DuckDBSuccess);
        duckdb_result result {};
        if (duckdb_execute_prepared(prepared, &result) != DuckDBSuccess) {
            std::fprintf(stderr, "%s\n%s\n", sql.c_str(), duckdb_result_error(&result));
            return 1;
        }
        auto chunk = duckdb_fetch_chunk(result);
        REQUIRE(chunk && duckdb_data_chunk_get_size(chunk) == 1);
        OK(pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(chunk, 0), 0, 1, &wrapped, &error));
        bool is_null = true;
        OK(pdd_bridge_value_render_pdo(wrapped, nullptr, &bytes, &is_null, &error));
        auto reference = static_cast<duckdb_string_t *>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1)));
        auto length = duckdb_string_t_length(*reference);
        if (bytes.length != length || std::memcmp(bytes.data, duckdb_string_t_data(reference), length)) {
            std::fprintf(stderr, "Geometry rendering differs: %s\n", sql.c_str());
            std::fprintf(stderr, "got: %.*s\nwant: %.*s\n", static_cast<int>(bytes.length), bytes.data,
                         static_cast<int>(length), duckdb_string_t_data(reference));
            return 1;
        }
        REQUIRE(!is_null);
        pdd_bridge_bytes_free(&bytes);
        pdd_bridge_value_destroy(&wrapped);
        duckdb_destroy_data_chunk(&chunk);
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&prepared);
    }
    auto null_value = duckdb_create_null_value();
    auto integer = duckdb_create_int32(3);
    auto hex_value = duckdb_create_varchar(hex);
    pdd_bridge_bytes empty_key {const_cast<char *>(""), 0};
    duckdb_value empty_object = nullptr, input_array = nullptr, expected_array = nullptr;
    OK(pdd_bridge_variant_object(&empty_key, &null_value, 1, &empty_object, &error));
    duckdb_value inputs[] = {geometry, integer, empty_object};
    duckdb_value expected[] = {hex_value, integer, empty_object};
    OK(pdd_bridge_variant_array(inputs, 3, &input_array, &error));
    OK(pdd_bridge_variant_array(expected, 3, &expected_array, &error));
    pdd_bridge_value *expected_wrapped = nullptr;
    OK(pdd_bridge_value_from_c(input_array, &wrapped, &error));
    OK(pdd_bridge_value_from_c(expected_array, &expected_wrapped, &error));
    bool is_null = true;
    pdd_bridge_bytes expected_bytes {};
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR < 2
    REQUIRE(pdd_bridge_value_render(expected_wrapped, nullptr, &expected_bytes, &is_null, &error) == PDD_BRIDGE_ERROR);
    REQUIRE(!is_null && expected_bytes.data == nullptr && error.data &&
            std::strstr(error.data, "Can't cast unnamed struct to VARIANT"));
    pdd_bridge_error_free(&error);
#else
    OK(pdd_bridge_value_render(expected_wrapped, nullptr, &expected_bytes, &is_null, &error));
    REQUIRE(!is_null);
    pdd_bridge_bytes_free(&expected_bytes);
#endif
    OK(pdd_bridge_value_render_pdo(wrapped, nullptr, &bytes, &is_null, &error));
    auto fallback_expected = duckdb::string("[") + hex + ", 3, {'': NULL}]";
    REQUIRE(!is_null && bytes.length == fallback_expected.size() &&
            !std::memcmp(bytes.data, fallback_expected.data(), bytes.length));
    pdd_bridge_bytes_free(&bytes);
    const char *containers[][2] = {
        {"[v]", "[r]"}, {"[v]::VARIANT[1]", "[r]::VARIANT[1]"},
        {"{'v':v,'s':'a,b'||chr(0)||'c'}", "{'v':r,'s':'a,b'||chr(0)||'c'}"},
        {"(v, 12.30::DECIMAL(9,2))", "(r, 12.30::DECIMAL(9,2))"},
        {"map(['key'],[v])", "map(['key'],[r])"},
        {"union_value(item := v)", "union_value(item := r)"}
    };
    for (auto &pair : containers) {
        auto sql = duckdb::string("WITH x AS (SELECT $1 AS v, 'RENDER_SENTINEL'::VARIANT AS r) SELECT ") +
                   pair[0] + ", CAST(" + pair[1] + " AS VARCHAR) FROM x";
        duckdb_prepared_statement prepared = nullptr;
        REQUIRE(duckdb_prepare(con, sql.c_str(), &prepared) == DuckDBSuccess);
        REQUIRE(duckdb_bind_value(prepared, 1, input_array) == DuckDBSuccess);
        duckdb_result result {};
        REQUIRE(duckdb_execute_prepared(prepared, &result) == DuckDBSuccess);
        auto chunk = duckdb_fetch_chunk(result);
        REQUIRE(chunk && duckdb_data_chunk_get_size(chunk) == 1);
        pdd_bridge_value *container = nullptr;
        OK(pdd_bridge_value_from_vector(duckdb_data_chunk_get_vector(chunk, 0), 0, 1, &container, &error));
        OK(pdd_bridge_value_render_pdo(container, nullptr, &bytes, &is_null, &error));
        auto reference = static_cast<duckdb_string_t *>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1)));
        duckdb::string wanted(duckdb_string_t_data(reference), duckdb_string_t_length(*reference));
        auto sentinel = wanted.find("RENDER_SENTINEL");
        REQUIRE(sentinel != duckdb::string::npos);
        wanted.replace(sentinel, sizeof("RENDER_SENTINEL") - 1, fallback_expected);
        REQUIRE(!is_null && bytes.length == wanted.size() && !std::memcmp(bytes.data, wanted.data(), bytes.length));
        pdd_bridge_bytes_free(&bytes);
        pdd_bridge_value_destroy(&container);
        duckdb_destroy_data_chunk(&chunk);
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&prepared);
    }
    pdd_bridge_variant *root = nullptr, *object = nullptr, *null_node = nullptr;
    OK(pdd_bridge_variant_from_c(input_array, &root, &error));
    pdd_bridge_variant_kind kind;
    idx_t count = 0;
    OK(pdd_bridge_variant_info(root, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_ARRAY && count == 3);
    OK(pdd_bridge_variant_child(root, 2, &object, &error));
    OK(pdd_bridge_variant_info(object, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_OBJECT && count == 1);
    OK(pdd_bridge_variant_key(object, 0, &bytes, &error));
    REQUIRE(bytes.length == 0);
    pdd_bridge_bytes_free(&bytes);
    OK(pdd_bridge_variant_child(object, 0, &null_node, &error));
    OK(pdd_bridge_variant_info(null_node, &kind, &count, &error));
    REQUIRE(kind == PDD_BRIDGE_VARIANT_NULL);
    pdd_bridge_variant_destroy(&null_node);
    pdd_bridge_variant_destroy(&object);
    pdd_bridge_variant_destroy(&root);
    pdd_bridge_value_destroy(&wrapped);
    pdd_bridge_value_destroy(&expected_wrapped);
    duckdb_destroy_value(&input_array);
    duckdb_destroy_value(&expected_array);
    duckdb_destroy_value(&empty_object);
    duckdb_destroy_value(&null_value);
    duckdb_destroy_value(&integer);
    duckdb_destroy_value(&hex_value);
    duckdb_destroy_value(&geometry);
    duckdb_disconnect(&con);
    duckdb_close(&db);
    std::puts("PASS: hexadecimal geometry in scalar and nested containers, VARIANT, NUL fields, and raw WKB bytes");
}
