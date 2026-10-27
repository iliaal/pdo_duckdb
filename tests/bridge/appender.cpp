#include "pdo_duckdb_bridge_appender.h"
#include "pdo_duckdb_bridge_build.h"

#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path = std::filesystem::temp_directory_path() / ("pdo-duckdb-appender-" + std::to_string(random()));
            if (std::filesystem::create_directory(path)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    Directory(const Directory &) = delete;
    Directory &operator=(const Directory &) = delete;
};

struct Database {
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;
    explicit Database(const std::string &path) {
        require(duckdb_open(path.c_str(), &database) == DuckDBSuccess, "Database open failed");
        if (duckdb_connect(database, &connection) != DuckDBSuccess) {
            duckdb_close(&database);
            throw std::runtime_error("Connection failed");
        }
    }
    ~Database() {
        duckdb_disconnect(&connection);
        duckdb_close(&database);
    }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
};

struct Result {
    duckdb_result value = {};
    Result(duckdb_connection connection, const char *sql) {
        if (duckdb_query(connection, sql, &value) != DuckDBSuccess) {
            std::string error = duckdb_result_error(&value);
            duckdb_destroy_result(&value);
            throw std::runtime_error(std::string(sql) + ": " + error);
        }
    }
    ~Result() {
        duckdb_destroy_result(&value);
    }
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;
};

void query(duckdb_connection connection, const char *sql) {
    Result result(connection, sql);
}

int64_t scalar(duckdb_connection connection, const char *sql) {
    Result result(connection, sql);
    require(duckdb_row_count(&result.value) == 1, "Expected one result row");
    return duckdb_value_int64(&result.value, 0, 0);
}

pdd_bridge_bytes bytes(const std::string &value) {
    return {const_cast<char *>(value.data()), value.size()};
}

struct Appender {
    duckdb_appender value = nullptr;
    Appender(duckdb_connection connection, const std::vector<std::string> &schema, const std::string &table) {
        std::vector<pdd_bridge_bytes> segments;
        for (const auto &segment : schema) {
            segments.push_back(bytes(segment));
        }
        pdd_bridge_error error = {};
        auto status = pdd_bridge_appender_create(connection, segments.data(), segments.size(), bytes(table), &value, &error);
        std::string message = error.data ? std::string(error.data, error.length) : "Appender creation failed without message";
        pdd_bridge_error_free(&error);
        require(status == PDD_BRIDGE_OK, message.c_str());
        require(value != nullptr, "Successful Appender is missing its C API handle");
    }
    ~Appender() {
        if (value) {
            duckdb_appender_destroy(&value);
        }
    }
    Appender(const Appender &) = delete;
    Appender &operator=(const Appender &) = delete;
    void row(int32_t number) {
        require(duckdb_append_int32(value, number) == DuckDBSuccess, "Integer append failed");
        require(duckdb_appender_end_row(value) == DuckDBSuccess, "End row failed");
    }
    void close() {
        require(duckdb_appender_destroy(&value) == DuckDBSuccess, "C API Appender destroy failed");
        require(value == nullptr, "C API destroy did not clear its handle");
    }
};

void error_test(duckdb_connection connection, const std::vector<std::string> &schema, const std::string &table,
                const char *expected = nullptr) {
    std::vector<pdd_bridge_bytes> segments;
    for (const auto &segment : schema) {
        segments.push_back(bytes(segment));
    }
    duckdb_appender appender = nullptr;
    pdd_bridge_error error = {};
    auto status = pdd_bridge_appender_create(connection, segments.data(), segments.size(), bytes(table), &appender, &error);
    require(status == PDD_BRIDGE_ERROR, "Invalid Appender request succeeded");
    require(appender == nullptr, "Failed creation published an Appender wrapper");
    std::string message = error.data ? std::string(error.data, error.length) : "";
    pdd_bridge_error_free(&error);
    require(!message.empty(), "Expected a diagnostic for invalid Appender request");
    if (expected) {
        require(message.find(expected) != std::string::npos, "Appender error did not identify the failed requirement");
    }
}

void common(duckdb_connection connection) {
    query(connection, "CREATE TABLE plain(i INTEGER)");
    query(connection, "CREATE SCHEMA \"literal.dot\"");
    query(connection, "CREATE TABLE \"literal.dot\".rows(i INTEGER)");
    query(connection, "CREATE SCHEMA \"quote\"\"schema\"");
    query(connection, "CREATE TABLE \"quote\"\"schema\".\"t\"\"able\"(i INTEGER)");
    for (auto number : {17, 18}) {
        Appender appender(connection, {}, "plain");
        appender.row(number);
        require(duckdb_appender_flush(appender.value) == DuckDBSuccess, "Explicit flush failed");
        appender.close();
    }
    Appender dotted(connection, {"literal.dot"}, "rows");
    dotted.row(23);
    dotted.close();
    Appender quoted(connection, {"quote\"schema"}, "t\"able");
    quoted.row(29);
    quoted.close();
    require(scalar(connection, "SELECT sum(i) FROM plain") == 35, "Default schema writes missing");
    require(scalar(connection, "SELECT i FROM \"literal.dot\".rows") == 23, "Literal dotted schema was misrouted");
    require(scalar(connection, "SELECT i FROM \"quote\"\"schema\".\"t\"\"able\"") == 29, "Quoted identifier was misrouted");
    error_test(connection, {""}, "plain", "empty");
    error_test(connection, {std::string("main\0ignored", 12)}, "plain", "NUL");
    error_test(connection, {}, std::string("plain\0ignored", 13), "NUL");
    error_test(connection, {}, "", "empty");
    error_test(connection, {"missing"}, "plain");
    error_test(connection, {}, "does_not_exist");
    error_test(nullptr, {}, "plain", "connection");
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
void nested(duckdb_connection connection) {
    query(connection, "CREATE SCHEMA literal");
    query(connection, "CREATE SCHEMA bridge.literal.dot");
    query(connection, "CREATE TABLE bridge.literal.dot.rows(i INTEGER)");
    Appender distinct(connection, {"literal", "dot"}, "rows");
    distinct.row(31);
    distinct.close();
    require(scalar(connection, "SELECT i FROM bridge.literal.dot.rows") == 31, "Nested path was not addressed");
    require(scalar(connection, "SELECT i FROM \"literal.dot\".rows") == 23, "Nested path overwrote literal dotted schema");

    query(connection, "CREATE SCHEMA outer_schema");
    query(connection, "CREATE SCHEMA bridge.outer_schema.\"inner.\"\"quoted\"");
    query(connection, "CREATE SEQUENCE sequence_value START 100");
    query(connection, "CREATE TABLE bridge.outer_schema.\"inner.\"\"quoted\".\"t.\"\"able\"("
                      "id INTEGER DEFAULT nextval('sequence_value'), \"key\"\"name\" INTEGER, "
                      "note VARCHAR DEFAULT 'retained', generated INTEGER GENERATED ALWAYS AS (\"key\"\"name\" * 2))");
    Appender subset(connection, {"outer_schema", "inner.\"quoted"}, "t.\"able");
    require(scalar(connection, "SELECT count(*) FROM duckdb_sequences() WHERE sequence_name='sequence_value' AND last_value IS NULL") == 1,
            "Appender creation evaluated a sequence default");
    require(duckdb_appender_add_column(subset.value, "key\"name") == DuckDBSuccess, "Quoted subset selection failed");
    subset.row(7);
    subset.row(8);
    require(duckdb_appender_flush(subset.value) == DuckDBSuccess, "Subset flush failed");
    require(duckdb_appender_add_column(subset.value, "key\"name") == DuckDBError, "Duplicate subset column accepted");
    require(duckdb_appender_add_column(subset.value, "generated") == DuckDBError, "Generated column accepted");
    require(duckdb_appender_add_column(subset.value, "missing") == DuckDBError, "Missing subset column accepted");
    require(duckdb_appender_clear_columns(subset.value) == DuckDBSuccess, "Clear subset failed");
    require(duckdb_appender_add_column(subset.value, "note") == DuckDBSuccess, "First reordered subset failed");
    require(duckdb_appender_add_column(subset.value, "key\"name") == DuckDBSuccess, "Second reordered subset failed");
    require(duckdb_append_default(subset.value) == DuckDBSuccess, "Foldable default append failed");
    subset.row(9);
    subset.close();
    require(scalar(connection, "SELECT count(*) FROM bridge.outer_schema.\"inner.\"\"quoted\".\"t.\"\"able\" "
                               "WHERE id IN (100,101,102) AND note='retained' AND generated=\"key\"\"name\"*2") == 3,
            "Subset order, generated value, sequence, or default was lost");
    require(scalar(connection, "SELECT currval('sequence_value')") == 102, "Sequence default was not evaluated per row");
    require(scalar(connection, "SELECT sum(\"key\"\"name\") FROM bridge.outer_schema.\"inner.\"\"quoted\".\"t.\"\"able\"") == 24,
            "Subset selection changed the appended values");

    query(connection, "BEGIN");
    Appender transactional(connection, {"literal", "dot"}, "rows");
    transactional.row(41);
    transactional.close();
    require(scalar(connection, "SELECT count(*) FROM bridge.literal.dot.rows") == 2, "Transaction append is invisible");
    query(connection, "ROLLBACK");
    require(scalar(connection, "SELECT count(*) FROM bridge.literal.dot.rows") == 1, "Appender committed its transaction");
    error_test(connection, {"literal", "absent"}, "rows");

    query(connection, "CREATE TABLE bridge.literal.dot.unique_rows(i INTEGER PRIMARY KEY)");
    Appender constraint(connection, {"literal", "dot"}, "unique_rows");
    constraint.row(5);
    require(duckdb_appender_flush(constraint.value) == DuckDBSuccess, "Initial unique row flush failed");
    constraint.row(5);
    require(duckdb_appender_flush(constraint.value) == DuckDBError, "Duplicate key flush succeeded");
    require(duckdb_appender_error(constraint.value) != nullptr, "Flush error has no C API diagnostic");
    require(duckdb_appender_destroy(&constraint.value) == DuckDBError, "Destroy lost the earlier flush failure");
    require(constraint.value == nullptr, "Failed Appender destroy leaked its handle");
    require(scalar(connection, "SELECT count(*) FROM bridge.literal.dot.unique_rows") == 1, "Failed flush lost committed rows");

    query(connection, "ATTACH ':memory:' AS alternate");
    query(connection, "CREATE SCHEMA alternate.top");
    query(connection, "CREATE SCHEMA alternate.top.child");
    query(connection, "CREATE TABLE alternate.top.child.rows(i INTEGER)");
    query(connection, "USE alternate");
    Appender alternate(connection, {"top", "child"}, "rows");
    query(connection, "USE bridge");
    alternate.row(53);
    alternate.close();
    require(scalar(connection, "SELECT i FROM alternate.top.child.rows") == 53, "Appender lost its creation-time catalog");
    query(connection, "DETACH alternate");
}

void disconnected() {
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;
    require(duckdb_open(nullptr, &database) == DuckDBSuccess, "Disconnected test open failed");
    require(duckdb_connect(database, &connection) == DuckDBSuccess, "Disconnected test connect failed");
    try {
        query(connection, "CREATE SCHEMA top");
        query(connection, "CREATE SCHEMA memory.top.child");
        query(connection, "CREATE TABLE memory.top.child.rows(i INTEGER)");
        Appender appender(connection, {"top", "child"}, "rows");
        appender.row(61);
        duckdb_disconnect(&connection);
        require(duckdb_appender_flush(appender.value) == DuckDBError, "Flush through a closed connection succeeded");
        require(duckdb_appender_destroy(&appender.value) == DuckDBError, "Closed-connection error was lost at destruction");
        require(appender.value == nullptr, "Closed-connection Appender leaked its handle");
    } catch (...) {
        duckdb_disconnect(&connection);
        duckdb_close(&database);
        throw;
    }
    duckdb_close(&database);
}
#endif

}

int main() {
    try {
        Directory directory;
        const auto path = (directory.path / "bridge.duckdb").string();
        {
            Database database(path);
            common(database.connection);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
            nested(database.connection);
#else
            error_test(database.connection, {"literal", "dot"}, "rows", "require DuckDB 2.0");
#endif
        }
        {
            Database reopened(path);
            require(scalar(reopened.connection, "SELECT sum(i) FROM plain") == 35, "Reopen lost default-schema rows");
            require(scalar(reopened.connection, "SELECT i FROM \"literal.dot\".rows") == 23, "Reopen lost literal-schema row");
            require(scalar(reopened.connection, "SELECT i FROM \"quote\"\"schema\".\"t\"\"able\"") == 29,
                    "Reopen lost quoted-schema row");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
            require(scalar(reopened.connection, "SELECT i FROM bridge.literal.dot.rows") == 31, "Reopen lost nested-schema row");
            require(scalar(reopened.connection, "SELECT count(*) FROM bridge.outer_schema.\"inner.\"\"quoted\".\"t.\"\"able\"") == 3,
                    "Reopen lost subset/default rows");
#endif
        }
        std::puts("PASS: literal/quoted schema Appenders, native C API lifecycle, invalid identifiers, durable writes");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        std::puts("PASS: nested schema paths, subset order, generated columns, sequence/default values, rollback");
        disconnected();
        std::puts("PASS: failed-flush diagnostics, retained catalog, closed-connection cleanup");
#else
        std::puts("PASS: nested schema capability fails explicitly on DuckDB 1.5");
#endif
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
