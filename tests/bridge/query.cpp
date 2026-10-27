#include "pdo_duckdb_bridge_query.h"
#include "pdo_duckdb_bridge_build.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void require(bool value, const char *message) {
	if (!value) {
		throw std::runtime_error(message);
	}
}

std::string sql_quote(const std::string &text) {
	std::string quoted = "'";
	for (char c : text) {
		quoted += c;
		if (c == '\'') {
			quoted += c;
		}
	}
	return quoted + "'";
}

struct TemporaryDirectory {
	std::filesystem::path path;
	TemporaryDirectory() {
		std::random_device random;
		for (int attempt = 0; attempt < 10; attempt++) {
			path = std::filesystem::temp_directory_path() / ("pdo-duckdb-query-" + std::to_string(random()));
			if (std::filesystem::create_directory(path)) {
				return;
			}
		}
		throw std::runtime_error("Cannot create unique test directory");
	}
	~TemporaryDirectory() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	TemporaryDirectory(const TemporaryDirectory &) = delete;
	TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
};

std::string read_file(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void checked(pdd_bridge_status status, pdd_bridge_error &error) {
	std::string message = error.data ? std::string(error.data, error.length) : "Bridge error without message";
	pdd_bridge_error_free(&error);
	if (status != PDD_BRIDGE_OK) {
		throw std::runtime_error(message);
	}
}

struct Database {
	duckdb_database db = nullptr;
	duckdb_connection conn = nullptr;
	Database() {
		require(duckdb_open(nullptr, &db) == DuckDBSuccess, "Open database failed");
		require(duckdb_connect(db, &conn) == DuckDBSuccess, "Connect failed");
	}
	~Database() {
		duckdb_disconnect(&conn);
		duckdb_close(&db);
	}
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
};

struct NativeResult {
	duckdb_result result = {};
	explicit NativeResult(duckdb_connection conn, const std::string &sql) {
		if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
			std::string message = duckdb_result_error(&result);
			duckdb_destroy_result(&result);
			throw std::runtime_error(sql + ": " + message);
		}
	}
	~NativeResult() {
		duckdb_destroy_result(&result);
	}
	NativeResult(const NativeResult &) = delete;
	NativeResult &operator=(const NativeResult &) = delete;
};

void query(duckdb_connection conn, const std::string &sql) {
	NativeResult result(conn, sql);
}

int64_t scalar(duckdb_connection conn, const std::string &sql) {
	NativeResult result(conn, sql);
	require(duckdb_row_count(&result.result) == 1, "Expected one scalar row");
	return duckdb_value_int64(&result.result, 0, 0);
}

std::string scalar_string(duckdb_connection conn, const std::string &sql) {
	NativeResult result(conn, sql);
	require(duckdb_row_count(&result.result) == 1, "Expected one scalar row");
	auto value = duckdb_value_string(&result.result, 0, 0);
	std::string text(value.data, value.size);
	duckdb_free(value.data);
	return text;
}

struct Result {
	pdd_bridge_result *result = nullptr;
	Result(duckdb_connection conn, const std::string &sql, bool unbuffered = false) {
		pdd_bridge_error error = {};
		checked(pdd_bridge_query_execute(conn, sql.data(), sql.size(), unbuffered, &result, &error), error);
	}
	~Result() {
		pdd_bridge_result_destroy(&result);
	}
	Result(const Result &) = delete;
	Result &operator=(const Result &) = delete;
	idx_t changed() {
		idx_t columns = 0, rows = 0;
		bool streaming = false;
		pdd_bridge_error error = {};
		checked(pdd_bridge_result_info(result, &columns, &rows, &streaming, &error), error);
		return rows;
	}
	bool streaming() {
		idx_t columns = 0, rows = 0;
		bool streaming = false;
		pdd_bridge_error error = {};
		checked(pdd_bridge_result_info(result, &columns, &rows, &streaming, &error), error);
		return streaming;
	}
	duckdb_data_chunk fetch() {
		duckdb_data_chunk chunk = nullptr;
		pdd_bridge_error error = {};
		checked(pdd_bridge_result_fetch(result, &chunk, &error), error);
		return chunk;
	}
};

pdd_bridge_sql_info classify(duckdb_connection conn, const std::string &sql) {
	pdd_bridge_sql_info info = {};
	pdd_bridge_error error = {};
	checked(pdd_bridge_query_classify(conn, sql.data(), sql.size(), &info, &error), error);
	return info;
}

void check_remote(duckdb_connection conn, bool expected) {
	bool connected = !expected;
	pdd_bridge_error error = {};
	checked(pdd_bridge_connection_is_remote(conn, &connected, &error), error);
	require(connected == expected, "Wrong remote state");
}

void check_remote_catalogs(duckdb_connection conn, bool expected) {
	bool attached = !expected;
	pdd_bridge_error error = {};
	checked(pdd_bridge_connection_has_remote_catalogs(conn, &attached, &error), error);
	require(attached == expected, "Wrong remote attachment state");
}

void changed(duckdb_connection conn, const std::string &sql, idx_t count, bool unbuffered = false) {
	Result result(conn, sql, unbuffered);
	require(result.changed() == count, "Incorrect affected-row count");
	require(!result.streaming(), "DML was left as a live stream");
}

void row_count_is_data(duckdb_connection conn, const std::string &sql, int32_t value) {
	Result result(conn, sql, true);
	require(result.changed() == 0, "RETURNING Count was interpreted as affected rows");
	require(!result.streaming(), "RETURNING DML was left as a live stream");
	duckdb_logical_type type = nullptr;
	pdd_bridge_bytes name = {};
	pdd_bridge_error error = {};
	checked(pdd_bridge_result_column(result.result, 0, &name, &type, &error), error);
	require(std::string(name.data, name.length) == "Count", "RETURNING column name was lost");
	require(duckdb_get_type_id(type) == DUCKDB_TYPE_INTEGER, "RETURNING column type was lost");
	pdd_bridge_bytes_free(&name);
	duckdb_destroy_logical_type(&type);
	auto chunk = result.fetch();
	require(chunk && duckdb_data_chunk_get_size(chunk) == 1, "RETURNING row was lost");
	auto vector = duckdb_data_chunk_get_vector(chunk, 0);
	require(static_cast<int32_t *>(duckdb_vector_get_data(vector))[0] == value, "RETURNING data changed");
	duckdb_destroy_data_chunk(&chunk);
	chunk = result.fetch();
	require(!chunk, "Unexpected RETURNING row");
}

pdd_bridge_exec_info batch(duckdb_connection conn, const std::string &sql, bool force_last_count = false,
	bool expect_error = false) {
	pdd_bridge_exec_info info = {};
	pdd_bridge_error error = {};
	auto status = pdd_bridge_exec_batch(conn, sql.data(), sql.size(), force_last_count, &info, &error);
	std::string message = error.data ? std::string(error.data, error.length) : "";
	pdd_bridge_error_free(&error);
	if (expect_error) {
		require(status == PDD_BRIDGE_ERROR && !message.empty(), "Batch failure lost its status or diagnostic");
	} else if (status != PDD_BRIDGE_OK) {
		throw std::runtime_error(sql + ": " + message);
	}
	return info;
}

void batch_counts(duckdb_connection conn, const std::string &sql, idx_t rows, idx_t completed,
	bool force_last_count = false, pdd_bridge_transaction_effect effect = PDD_BRIDGE_TRANSACTION_NONE) {
	auto info = batch(conn, sql, force_last_count);
	if (info.rows_changed != rows || info.completed_statements != completed || info.last_transaction_effect != effect) {
		throw std::runtime_error(sql + ": incorrect batch progress (rows=" + std::to_string(info.rows_changed) +
			", completed=" + std::to_string(info.completed_statements) +
			", transaction=" + std::to_string(info.last_transaction_effect) + ")");
	}
}

void batch_test(duckdb_connection conn, duckdb_connection observer) {
	bool remote = false, remote_catalogs = false;
	pdd_bridge_error error = {};
	checked(pdd_bridge_connection_is_remote(conn, &remote, &error), error);
	checked(pdd_bridge_connection_has_remote_catalogs(conn, &remote_catalogs, &error), error);
	bool local_prepare = !remote && !remote_catalogs;
	query(conn, "CREATE TABLE bridge_batch(i INTEGER PRIMARY KEY,s VARCHAR)");
	batch_counts(conn, "  -- empty ;\n /* no statement ; */ ", 0, 0);
	batch_counts(conn, "INSERT INTO bridge_batch VALUES(1,'one'); INSERT INTO bridge_batch VALUES(2,'two'),(3,'three')", 2, 2);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 3 &&
		scalar(observer, "SELECT sum(i) FROM bridge_batch") == 6, "DML batch duplicated or lost writes");
	batch_counts(conn, "UPDATE bridge_batch SET i=i+10 WHERE i IN (1,2); DELETE FROM bridge_batch WHERE i=3", 1, 2);
	batch_counts(conn, "MERGE INTO bridge_batch t USING (VALUES(12),(13)) s(i) ON t.i=s.i "
		"WHEN MATCHED THEN UPDATE SET s='merged' WHEN NOT MATCHED THEN INSERT VALUES(s.i,'new'); "
		"UPDATE bridge_batch SET s='changed' WHERE i=11", 1, 2);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 3 &&
		scalar(observer, "SELECT sum(i) FROM bridge_batch") == 36, "MERGE batch was replayed or skipped");
	batch_counts(conn, "SELECT 999999::BIGINT AS Count; INSERT INTO bridge_batch VALUES(20,'selected')", 0, 2);
	batch_counts(conn, "INSERT INTO bridge_batch VALUES(21,'returning') RETURNING 999999::BIGINT AS Count; "
		"INSERT INTO bridge_batch VALUES(22,'after-returning')", 0, 2);
	batch_counts(conn, "UPDATE bridge_batch SET s='updated' WHERE i=21 RETURNING 999999::BIGINT AS Count; "
		"DELETE FROM bridge_batch WHERE i=22", 0, 2);
	batch_counts(conn, "DELETE FROM bridge_batch WHERE i=21 RETURNING 999999::BIGINT AS Count; "
		"INSERT INTO bridge_batch VALUES(23,'after-delete')", 0, 2);
	batch_counts(conn, "MERGE INTO bridge_batch t USING (SELECT 23 i) s ON t.i=s.i "
		"WHEN MATCHED THEN UPDATE SET s='returned-merge' RETURNING 999999::BIGINT AS Count; "
		"INSERT INTO bridge_batch VALUES(24,'after-merge')", 0, 2);
	batch_counts(conn, "SELECT 999999::BIGINT AS Count; INSERT INTO bridge_batch VALUES(25,'forced'),(26,'forced')", 2, 2, true);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 8, "Result-returning batch lost committed writes");
	query(conn, "DELETE FROM bridge_batch");
	batch_counts(conn, "/* ; outer /* ; nested */ */ INSERT INTO bridge_batch VALUES(1,'a;''b'); -- ; ignored\n"
		"INSERT INTO bridge_batch VALUES(2,$quoted$dollar; 'quote' -- /*$quoted$); "
		"UPDATE bridge_batch SET s=s||';tail' WHERE i=1", 1, 3);
	require(scalar_string(observer, "SELECT s FROM bridge_batch WHERE i=1") == "a;'b;tail" &&
		scalar_string(observer, "SELECT s FROM bridge_batch WHERE i=2") == "dollar; 'quote' -- /*",
		"Batch splitting changed quoted strings or comment boundaries");
	batch_counts(conn, "CREATE TABLE \"bridge;quoted\"(\"field;name\" INTEGER); "
		"INSERT INTO \"bridge;quoted\" VALUES(17)", 1, 2);
	require(scalar(observer, "SELECT \"field;name\" FROM \"bridge;quoted\"") == 17, "Quoted identifier was split at its semicolon");
	query(conn, "DROP TABLE \"bridge;quoted\"");
	query(conn, "SET preserve_identifier_case=true");
	query(conn, "SET preserve_identifier_case=false; CREATE TABLE bridge_batch_native_snapshot(MiXeD INTEGER)");
	auto native_identifier = scalar_string(observer,
		"SELECT column_name FROM information_schema.columns WHERE table_name='bridge_batch_native_snapshot'");
	query(conn, "DROP TABLE bridge_batch_native_snapshot");
	query(conn, "SET preserve_identifier_case=true");
	batch_counts(conn, "SET preserve_identifier_case=false; CREATE TABLE bridge_batch_snapshot(MiXeD INTEGER)", 0, 2);
	require(scalar_string(observer, "SELECT column_name FROM information_schema.columns WHERE table_name='bridge_batch_snapshot'") == native_identifier,
		"Batch changed parser options after parsing had started");
	query(conn, "DROP TABLE bridge_batch_snapshot");
	batch_counts(conn, "CREATE TABLE bridge_batch_case(MiXeD INTEGER); INSERT INTO bridge_batch_case VALUES(17)", 1, 2);
	require(scalar_string(observer, "SELECT column_name FROM information_schema.columns WHERE table_name='bridge_batch_case'") == "mixed",
		"Batch parser ignored the executing connection's identifier-case option");
	query(conn, "DROP TABLE bridge_batch_case");
	batch_counts(conn, "SET preserve_identifier_case=true", 0, 1);
	query(conn, "DELETE FROM bridge_batch");
	auto failed = batch(conn, "INSERT INTO bridge_batch VALUES(30,$tail$prefix; 'quote' -- ;$tail$); /* tail ; */ SELEC", false, true);
	require(failed.parse_failed, "Parser failure lost its error phase");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	require(failed.completed_statements == 1 && scalar(observer, "SELECT count(*) FROM bridge_batch WHERE i=30") == 1,
		"GA lazy parse lost or replayed a committed prefix");
	require(scalar_string(observer, "SELECT s FROM bridge_batch WHERE i=30") == "prefix; 'quote' -- ;",
		"Malformed tail changed a committed quoted prefix");
#else
	require(failed.completed_statements == 0 && scalar(observer, "SELECT count(*) FROM bridge_batch") == 0,
		"Legacy eager parse executed a prefix before rejecting invalid SQL");
#endif
	require(failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_NONE, "Parser failure invented transaction control");
	query(conn, "DELETE FROM bridge_batch");
	failed = batch(conn, "INSERT INTO bridge_batch VALUES(31,'catalog-prefix'); SELECT * FROM bridge_batch_missing", false, true);
	require(!failed.parse_failed && !failed.prepare_failed, "Ordinary catalog error changed its execution phase");
	require(failed.completed_statements == 1 && scalar(observer, "SELECT count(*) FROM bridge_batch WHERE i=31") == 1,
		"Catalog failure lost or replayed a committed prefix");
	query(conn, "DELETE FROM bridge_batch");
	failed = batch(conn, "BEGIN; INSERT INTO bridge_batch VALUES(40,'must-not-run'); SELEC", true, true);
	require(failed.parse_failed, "Eager parser failure lost its error phase");
	require(failed.completed_statements == 0 && failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_NONE &&
		scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Forced batch executed before eager parse completed");
	batch_counts(conn, "BEGIN; INSERT INTO bridge_batch VALUES(41,'transaction'); SELECT 1; "
		"INSERT INTO bridge_batch VALUES(42,'transaction'),(43,'transaction')", 2, 4, true, PDD_BRIDGE_TRANSACTION_BEGIN);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Batch committed an explicit transaction");
	batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Batch rollback retained writes");
	for (const char *end : {"COMMIT", "ROLLBACK"}) {
		failed = batch(conn, std::string("BEGIN; INSERT INTO bridge_batch VALUES(50,'control'); ") + end +
			"; SELECT * FROM bridge_batch_missing", true, true);
		require(failed.completed_statements == 3 && failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_END,
			"Later failure lost a completed transaction end");
		require(scalar(observer, "SELECT count(*) FROM bridge_batch") == (std::string(end) == "COMMIT" ? 1 : 0),
			"Completed batch transaction control had the wrong effect");
		query(conn, "DELETE FROM bridge_batch");
	}
	failed = batch(conn, "BEGIN; INSERT INTO bridge_batch VALUES(51,'pending'); SELECT * FROM bridge_batch_missing", true, true);
	require(!failed.parse_failed && failed.prepare_failed == local_prepare,
		"Forced catalog failure lost its local preparation phase");
	require(failed.completed_statements == 2 && failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_BEGIN,
		"Later failure lost a completed transaction begin");
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Failed batch committed pending writes");
	batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
	failed = batch(conn, "BEGIN; SELECT missing_column FROM bridge_batch", true, true);
	require(!failed.parse_failed && failed.prepare_failed == local_prepare && failed.completed_statements == 1,
		"Forced binder failure lost its local preparation phase");
	batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
	failed = batch(conn, "BEGIN; INSERT INTO bridge_batch VALUES(52,'first'); INSERT INTO bridge_batch VALUES(52,'duplicate')", true, true);
	require(!failed.parse_failed && !failed.prepare_failed && failed.completed_statements == 2 &&
		failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_BEGIN,
		"Forced constraint failure was mistaken for a preparation failure");
	batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
	require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Forced execution failure retained transaction writes");
	for (bool force : {false, true}) {
		batch_counts(conn, "BEGIN; INSERT INTO bridge_batch VALUES(60,'parser-before')", 1, 2, true, PDD_BRIDGE_TRANSACTION_BEGIN);
		failed = batch(conn, "SELEC", force, true);
		require(failed.completed_statements == 0 && failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_NONE,
			"Standalone parser error changed batch progress");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		if (!force) {
			failed = batch(conn, "INSERT INTO bridge_batch VALUES(61,'must-be-aborted')", false, true);
			require(failed.completed_statements == 0, "GA query parser error did not invalidate the executing transaction");
		} else {
			batch_counts(conn, "INSERT INTO bridge_batch VALUES(61,'extraction-still-valid')", 1, 1);
		}
#else
		batch_counts(conn, "INSERT INTO bridge_batch VALUES(61,'legacy-still-valid')", 1, 1);
#endif
		batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
		require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Parser failure cleanup retained transaction writes");
	}
	for (const auto &invalid : {std::string("SELECT 'unterminated"), std::string("SELECT /* unterminated"),
		std::string("SELECT '\xff'"), std::string("SELECT CAST(1 AS DECIMAL(2,3))")}) {
		query(conn, "BEGIN");
		bool native_failed = false;
		try {
			query(conn, invalid);
		} catch (const std::exception &) {
			native_failed = true;
		}
		bool native_aborted = false;
		try {
			query(conn, "INSERT INTO bridge_batch VALUES(62,'native-parser-probe')");
		} catch (const std::exception &) {
			native_aborted = true;
		}
		query(conn, "ROLLBACK");
		require(native_failed, "Invalid parser fixture unexpectedly succeeded natively");
		batch_counts(conn, "BEGIN; INSERT INTO bridge_batch VALUES(60,'parser-before')", 1, 2, true,
			PDD_BRIDGE_TRANSACTION_BEGIN);
		failed = batch(conn, invalid, false, true);
		require(failed.completed_statements == 0, "Parser failure reported a completed statement");
		bool bridge_aborted = false;
		try {
			query(conn, "INSERT INTO bridge_batch VALUES(62,'bridge-parser-probe')");
		} catch (const std::exception &) {
			bridge_aborted = true;
		}
		query(conn, "ROLLBACK");
		require(bridge_aborted == native_aborted, "Batch parser failure changed native transaction invalidation");
		require(scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Parser comparison retained transaction writes");
	}
	for (const char *end : {"", "COMMIT; ", "ROLLBACK; "}) {
		failed = batch(conn, std::string("BEGIN; INSERT INTO bridge_batch VALUES(65,'lazy-control'); ") + end + "SELEC", false, true);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		require(failed.completed_statements == (*end ? idx_t(3) : idx_t(2)) &&
			failed.last_transaction_effect == (*end ? PDD_BRIDGE_TRANSACTION_END : PDD_BRIDGE_TRANSACTION_BEGIN),
			"GA parser failure lost completed transaction control");
		require(scalar(observer, "SELECT count(*) FROM bridge_batch") == (std::string(end) == "COMMIT; " ? 1 : 0),
			"GA parser failure changed a completed transaction's effect");
		if (!*end) {
			batch_counts(conn, "ROLLBACK", 0, 1, false, PDD_BRIDGE_TRANSACTION_END);
		}
#else
		require(failed.completed_statements == 0 && failed.last_transaction_effect == PDD_BRIDGE_TRANSACTION_NONE &&
			scalar(observer, "SELECT count(*) FROM bridge_batch") == 0, "Legacy parser failure executed transaction controls");
#endif
		query(conn, "DELETE FROM bridge_batch");
	}
	const char nul_sql[] = "INSERT INTO bridge_batch VALUES(70,'prefix')\0; INSERT INTO bridge_batch VALUES(71,'suffix')";
	failed = batch(conn, std::string(nul_sql, sizeof(nul_sql) - 1), false, true);
	require(failed.completed_statements == 0 && scalar(observer, "SELECT count(*) FROM bridge_batch") == 0,
		"Embedded NUL batch executed a truncated query");
	query(conn, "DROP TABLE bridge_batch");
}

void session_variables_test(duckdb_connection conn) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	{
		Result set(conn, "SET VARIABLE bridge_session=7");
	}
	require(scalar(conn, "SELECT $bridge_session") == 7, "Native session variable setup failed");
	auto info = classify(conn, "SELECT $bridge_session + $bound + $bound");
	require(info.kind == PDD_BRIDGE_SQL_SELECT && info.parameter_count == 2,
		"Session variable or repeated parameter reference was lost");
	for (bool streaming : {false, true}) {
		Result selected(conn, "SELECT $bridge_session + 17 + 17 WHERE $bridge_session=7", streaming);
		auto chunk = selected.fetch();
		require(chunk && duckdb_data_chunk_get_size(chunk) == 1, "Session variable SELECT lost its row");
		auto vector = duckdb_data_chunk_get_vector(chunk, 0);
		require(static_cast<int32_t *>(duckdb_vector_get_data(vector))[0] == 41,
			"Session variable did not coexist with expanded repeated bindings");
		duckdb_destroy_data_chunk(&chunk);
	}
	{
		Result set(conn, "SET VARIABLE bridge_session=9");
	}
	{
		Result selected(conn, "SELECT $bridge_session + 33");
		auto chunk = selected.fetch();
		require(chunk && duckdb_data_chunk_get_size(chunk) == 1, "Updated variable SELECT lost its row");
		auto vector = duckdb_data_chunk_get_vector(chunk, 0);
		require(static_cast<int32_t *>(duckdb_vector_get_data(vector))[0] == 42,
			"Execution reused a stale session variable value");
		duckdb_destroy_data_chunk(&chunk);
	}
#else
	(void) conn;
#endif
}

void diagnostic_test(duckdb_connection conn) {
	for (bool json : {false, true}) {
		query(conn, std::string("SET errors_as_json=") + (json ? "true" : "false"));
		for (const char *sql : {"SELECT 1 FROM", "", "SELECT 1; SELECT 2"}) {
			duckdb_prepared_statement prepared = nullptr;
			require(duckdb_prepare(conn, sql, &prepared) == DuckDBError, "Native diagnostic fixture succeeded");
			std::string native = duckdb_prepare_error(prepared);
			duckdb_destroy_prepare(&prepared);
			pdd_bridge_sql_info info = {};
			pdd_bridge_error error = {};
			auto status = pdd_bridge_query_classify(conn, sql, std::strlen(sql), &info, &error);
			std::string message = error.data ? std::string(error.data, error.length) : "";
			pdd_bridge_error_free(&error);
			require(status == PDD_BRIDGE_ERROR && message == native, "Parse-only diagnostic differs from native prepare");
			if (std::strcmp(sql, "SELECT 1 FROM") == 0) {
				require(message.find(json ? "Parser Error: {\"exception_type\":\"Parser\"" : "Parser Error:") == 0,
					"Parser diagnostic ignored errors_as_json");
				pdd_bridge_exec_info progress = {};
				status = pdd_bridge_exec_batch(conn, sql, std::strlen(sql), true, &progress, &error);
				message = error.data ? std::string(error.data, error.length) : "";
				pdd_bridge_error_free(&error);
				if (status != PDD_BRIDGE_ERROR || !progress.parse_failed || message != native) {
					throw std::runtime_error("Eager batch diagnostic differs from native prepare: got [" + message +
						"], native [" + native + "]");
				}
			}
		}
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		Result stream(conn, "SELECT i FROM range(5000) t(i)", true);
		duckdb_prepared_statement prepared = nullptr;
		require(duckdb_prepare(conn, "SELECT 7", &prepared) == DuckDBError, "Native live-result prepare was accepted");
		std::string native = duckdb_prepare_error(prepared);
		duckdb_destroy_prepare(&prepared);
		pdd_bridge_sql_info info = {};
		pdd_bridge_error error = {};
		auto status = pdd_bridge_query_classify(conn, "SELECT 7", 8, &info, &error);
		std::string message = error.data ? std::string(error.data, error.length) : "";
		pdd_bridge_error_free(&error);
		require(status == PDD_BRIDGE_ERROR && message == native, "Live-result diagnostic differs from native prepare");
		idx_t rows = 0;
		while (auto chunk = stream.fetch()) {
			rows += duckdb_data_chunk_get_size(chunk);
			duckdb_destroy_data_chunk(&chunk);
		}
		require(rows == 5000, "Formatting a refused prepare invalidated the original stream");
#endif
	}
	query(conn, "SET errors_as_json=false");
}

void common_query_test(duckdb_connection conn, duckdb_connection observer, const std::string &table) {
	session_variables_test(conn);
	auto info = classify(conn, "INSERT INTO " + table + " VALUES (999,'must not execute',NULL)");
	require(info.kind == PDD_BRIDGE_SQL_INSERT && !info.has_returning, "Incorrect literal INSERT classification");
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0, "Classify executed literal INSERT");
	info = classify(conn, "INSERT INTO " + table + " VALUES ($1,$2,$3)");
	require(info.kind == PDD_BRIDGE_SQL_INSERT && info.parameter_count == 3 && !info.has_returning,
		"Incorrect INSERT classification");
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0, "Classify executed INSERT");
	info = classify(conn, "SELECT * FROM " + table + " WHERE i=$needle OR i=$needle");
	require(info.parameter_count == 1 && info.kind == PDD_BRIDGE_SQL_SELECT, "Repeated binding was counted twice");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	info = classify(conn, "ATTACH $1 AS q");
	require(info.parameter_count == 1, "ATTACH parameter was lost");
#endif
	const std::string unsafe = "INSERT INTO " + table + " VALUES ($1,$2,$3)";
	pdd_bridge_error error = {};
	pdd_bridge_result *bad_result = nullptr;
	require(pdd_bridge_query_execute(conn, unsafe.data(), unsafe.size(), false, &bad_result, &error) == PDD_BRIDGE_ERROR,
		"Unexpanded bindings were accepted");
	require(!bad_result && error.length, "Unexpanded binding error lost diagnostic");
	pdd_bridge_error_free(&error);
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0, "Rejected execution changed rows");
	changed(conn, "INSERT INTO " + table + " VALUES (42,decode(from_hex('610062273b44524f50205441424c4520783b2d2d')),from_hex('00ff78'))", 1, true);
	require(scalar_string(observer, "SELECT hex(s)||':'||hex(b) FROM bridge_query_t") ==
		"610062273B44524F50205441424C4520783B2D2D:00FF78", "NUL or binary binding bytes changed");
	{
		Result result(conn, "SELECT i FROM " + table + " WHERE i=42 OR i=42");
		require(result.changed() == 0, "SELECT reported affected rows");
		auto chunk = result.fetch();
		require(chunk && duckdb_data_chunk_get_size(chunk) == 1, "Repeated binding SELECT failed");
		duckdb_destroy_data_chunk(&chunk);
	}
	changed(conn, "UPDATE " + table + " SET i=43 WHERE i=42", 1);
	row_count_is_data(conn, "UPDATE " + table + " SET i=42 WHERE i=43 RETURNING i AS Count", 42);
	changed(conn, "MERGE INTO " + table + " t USING (SELECT 42 AS i) s ON t.i=s.i WHEN MATCHED THEN UPDATE SET s='merged'", 1);
	{
		Result begin(conn, "BEGIN");
	}
	changed(conn, "INSERT INTO " + table + " VALUES (99,'transaction',NULL)", 1);
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 1, "Uncommitted remote row became visible");
	{
		Result rollback(conn, "ROLLBACK");
	}
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 1, "Rollback kept inserted row");
	changed(conn, "DELETE FROM " + table + " WHERE i=42", 1);
	changed(conn, "DELETE FROM " + table + " WHERE i=42", 0);
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0, "Committed DELETE did not persist");
}

void refused_classification(duckdb_connection conn) {
	bool legacy = false;
	pdd_bridge_error mode_error = {};
	checked(pdd_bridge_connection_uses_legacy_parser(conn, &legacy, &mode_error), mode_error);
	require(legacy == (PDO_DUCKDB_BRIDGE_ENGINE_MAJOR < 2), "Parser mode lookup changed while a result was live");
	auto mode_status = pdd_bridge_query_uses_legacy_parser(conn, "SELECT $1_0", 11, &legacy, &mode_error);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	std::string mode_message = mode_error.data ? std::string(mode_error.data, mode_error.length) : "";
	pdd_bridge_error_free(&mode_error);
	require(mode_status == PDD_BRIDGE_ERROR && mode_message.find("connection has an open result") != std::string::npos,
		"Query parser probe bypassed live-result admission");
	for (const char *sql : {"SELECT 7", "SELECT 1 FROM"}) {
		pdd_bridge_sql_info info = {};
		pdd_bridge_error error = {};
		auto status = pdd_bridge_query_classify(conn, sql, std::strlen(sql), &info, &error);
		std::string message = error.data ? std::string(error.data, error.length) : "";
		pdd_bridge_error_free(&error);
		require(status == PDD_BRIDGE_ERROR && message.find("connection has an open result") != std::string::npos,
			"Parse-only classification bypassed live-result admission");
	}
#else
	checked(mode_status, mode_error);
	require(legacy, "Legacy parser probe changed grammar while a result was live");
#endif
}

void classify_transaction_test(duckdb_connection conn, duckdb_connection observer, const std::string &table,
	bool connected = false, const char *sql = "SELECT 1 FROM") {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	for (const char *policy : {"STANDARD_POLICY", "SYNTACTIC_ERRORS_DO_NOT_INVALIDATE", "ALL_ERRORS_INVALIDATE_TRANSACTION"}) {
#else
	for (const char *policy : {"legacy"}) {
#endif
		for (bool bridge : {false, true}) {
			query(conn, "BEGIN");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
			query(conn, "SET current_transaction_invalidation_policy=" + sql_quote(policy));
#endif
			query(conn, "INSERT INTO " + table + " VALUES(17,'classify-transaction',NULL)");
			if (bridge) {
				pdd_bridge_sql_info info = {};
				pdd_bridge_error error = {};
				auto status = pdd_bridge_query_classify(conn, sql, std::strlen(sql), &info, &error);
				bool diagnostic = error.data && error.length;
				pdd_bridge_error_free(&error);
				require(status == PDD_BRIDGE_ERROR && diagnostic, "Malformed classification did not fail");
				if (connected) {
					auto insert_info = classify(conn, "INSERT INTO " + table + " VALUES(999,'must-not-execute',NULL)");
					require(insert_info.kind == PDD_BRIDGE_SQL_INSERT, "Connected INSERT classification failed");
					query(conn, "INSERT INTO " + table + " VALUES(18,'server-transaction-still-usable',NULL)");
					require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0,
						"Client validation committed the server transaction");
				}
			} else if (connected) {
				bool rejected = false;
				try {
					query(conn, sql);
				} catch (const std::exception &) {
					rejected = true;
				}
				require(rejected, "Malformed server execution did not fail");
			} else {
				duckdb_prepared_statement prepared = nullptr;
				auto status = duckdb_prepare(conn, sql, &prepared);
				duckdb_destroy_prepare(&prepared);
				require(status == DuckDBError, "Malformed native prepare did not fail");
			}
			query(conn, "COMMIT");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
			int64_t expected = std::strcmp(policy, "SYNTACTIC_ERRORS_DO_NOT_INVALIDATE") == 0 &&
				std::strcmp(sql, "SELECT 1 FROM") == 0 ? 1 : 0;
			if (connected && bridge) {
				expected = 2;
			}
#else
			(void) policy;
			int64_t expected = 1;
#endif
			auto rows = scalar(observer, "SELECT count(*) FROM bridge_query_t");
			if (rows != expected) {
				throw std::runtime_error("Classification changed transaction policy " + std::string(policy) +
					" for SQL '" + sql + "' (bridge=" + std::to_string(bridge) +
					", connected=" + std::to_string(connected) + ", rows=" + std::to_string(rows) +
					", expected=" + std::to_string(expected) + ")");
			}
			query(conn, "DELETE FROM " + table);
		}
	}
}

void stream_test(duckdb_connection conn) {
	Result stream(conn, "SELECT i FROM range(100000) t(i)", true);
	require(stream.streaming(), "Requested SELECT did not produce a streaming result");
	auto chunk = stream.fetch();
	require(chunk && duckdb_data_chunk_get_size(chunk) > 0 && duckdb_data_chunk_get_size(chunk) < 100000,
		"Streaming fetch did not return a bounded chunk");
	idx_t rows = duckdb_data_chunk_get_size(chunk);
	duckdb_destroy_data_chunk(&chunk);
	refused_classification(conn);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	pdd_bridge_error error = {};
	pdd_bridge_result *refused = nullptr;
	const char sql[] = "SELECT 7";
	require(pdd_bridge_query_execute(conn, sql, sizeof(sql) - 1, false, &refused, &error) == PDD_BRIDGE_ERROR,
		"Concurrent query was accepted while GA stream was live");
	require(!refused && error.length, "Refused query lost its error");
	pdd_bridge_error_free(&error);
#endif
	while ((chunk = stream.fetch())) {
		rows += duckdb_data_chunk_get_size(chunk);
		duckdb_destroy_data_chunk(&chunk);
	}
	require(rows == 100000, "Original stream lost rows after refused query");
}

void other_affected_rows_test(duckdb_connection conn) {
	TemporaryDirectory temporary;
	auto path = temporary.path / "rows.csv";
	auto returned_path = temporary.path / "returned.csv";
	changed(conn, "CREATE TABLE bridge_ctas AS SELECT i FROM range(3) t(i)", 3);
	changed(conn, "CREATE TABLE IF NOT EXISTS bridge_ctas AS SELECT i FROM range(7) t(i)", 0);
	require(scalar(conn, "SELECT count(*) FROM bridge_ctas") == 3, "No-op CTAS changed existing table");
	changed(conn, "COPY bridge_ctas TO " + sql_quote(path.string()) + " (FORMAT CSV)", 3);
	require(std::filesystem::file_size(path) > 0, "COPY did not create its output");
	query(conn, "CREATE TABLE bridge_copy(i BIGINT)");
	changed(conn, "COPY bridge_copy FROM " + sql_quote(path.string()) + " (FORMAT CSV)", 3);
	require(scalar(conn, "SELECT count(*) FROM bridge_copy") == 3, "COPY rows were lost");
	{
		Result returning(conn, "COPY bridge_ctas TO " + sql_quote(returned_path.string()) + " (FORMAT CSV, RETURN_FILES)");
		require(returning.changed() == 0, "COPY RETURN_FILES result was confused with a count result");
		auto chunk = returning.fetch();
		require(chunk && duckdb_data_chunk_get_column_count(chunk) == 2, "COPY RETURN_FILES data was lost");
		duckdb_destroy_data_chunk(&chunk);
	}
	query(conn, "DROP TABLE bridge_ctas");
	query(conn, "DROP TABLE bridge_copy");
}

void configure_writers(duckdb_connection conn, const TemporaryDirectory &temporary) {
	auto profile = temporary.path / "profile.json";
	auto log = temporary.path / "query.log";
	query(conn, "PRAGMA enable_profiling='json'");
	query(conn, "SET profiling_output=" + sql_quote(profile.string()));
	query(conn, "SET log_query_path=" + sql_quote(log.string()));
	query(conn, "SELECT 42 AS writer_positive_control");
	require(read_file(profile).find("writer_positive_control") != std::string::npos,
		"Profiling control did not write a file");
	require(read_file(log).find("writer_positive_control") != std::string::npos,
		"Query logging control did not write a file");
}

void writer_test(duckdb_connection conn, duckdb_connection observer, const std::string &table,
	const TemporaryDirectory *existing = nullptr) {
	TemporaryDirectory temporary;
	if (!existing) {
		configure_writers(conn, temporary);
	}
	auto profile = (existing ? existing->path : temporary.path) / "profile.json";
	auto log = (existing ? existing->path : temporary.path) / "query.log";
	query(conn, "BEGIN");
	changed(conn, "INSERT INTO " + table + " VALUES(17,'uncommitted',NULL)", 1);
	{
		Result stream(conn, "SELECT i AS after_writer_cleanup FROM range(100000) t(i)", true);
		auto chunk = stream.fetch();
		require(chunk != nullptr, "Writer test stream returned no rows");
		idx_t rows = duckdb_data_chunk_get_size(chunk);
		duckdb_destroy_data_chunk(&chunk);
		refused_classification(conn);
		auto old_profile = read_file(profile);
		auto old_log = read_file(log);
		pdd_bridge_error error = {};
		checked(pdd_bridge_clear_local_writers(conn, &error), error);
		while ((chunk = stream.fetch())) {
			rows += duckdb_data_chunk_get_size(chunk);
			duckdb_destroy_data_chunk(&chunk);
		}
		require(rows == 100000, "Clearing local writers disrupted the live stream");
		require(read_file(profile) == old_profile, "Live result wrote profiling output after cleanup");
		require(read_file(log) == old_log, "Cleanup wrote query-log bytes");
		query(conn, "SELECT 123 AS after_writer_cleanup");
		require(read_file(profile) == old_profile, "Later query restarted profiling output");
		require(read_file(log) == old_log, "Later query restarted query logging");
	}
	require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0,
		"Clearing local writers committed an active transaction");
	query(conn, "COMMIT");
	require(scalar(observer, "SELECT i FROM bridge_query_t") == 17,
		"Clearing local writers lost the active transaction");
	query(conn, "DELETE FROM " + table);
}

void local_test() {
	Database db;
	duckdb_connection observer = nullptr;
	require(duckdb_connect(db.db, &observer) == DuckDBSuccess, "Observer connect failed");
	try {
		query(db.conn, "SET threads=1");
		query(db.conn, "CREATE TABLE bridge_query_t(i INTEGER PRIMARY KEY,s VARCHAR,b BLOB)");
		check_remote(db.conn, false);
		diagnostic_test(db.conn);
		check_remote_catalogs(db.conn, false);
		query(db.conn, "ATTACH ':memory:' AS bridge_local_attachment");
		check_remote_catalogs(db.conn, false);
		query(db.conn, "DETACH bridge_local_attachment");
		check_remote_catalogs(db.conn, false);
		common_query_test(db.conn, observer, "bridge_query_t");
		for (const char *sql : {"SELECT 1 FROM", "", "SELECT 1; SELECT 2"}) {
			classify_transaction_test(db.conn, observer, "bridge_query_t", false, sql);
		}
		batch_test(db.conn, observer);
		other_affected_rows_test(db.conn);
		stream_test(db.conn);
		writer_test(db.conn, observer, "bridge_query_t");
		{
			Result lazy(db.conn, "SELECT CASE WHEN i=100000000 THEN error('late error') ELSE i END FROM range(100000001) t(i)", true);
			require(lazy.streaming(), "Late-error SELECT was materialized");
			auto chunk = lazy.fetch();
			require(chunk && duckdb_data_chunk_get_size(chunk) > 0, "Lazy SELECT failed before the late error");
			duckdb_destroy_data_chunk(&chunk);
		}
		require(scalar(db.conn, "SELECT 8") == 8, "Destroying stream left connection unusable");
		const char bad_sql[] = "SELECT 1\0;INSERT INTO bridge_query_t VALUES(1,NULL,NULL)";
		pdd_bridge_sql_info info = {};
		pdd_bridge_error error = {};
		require(pdd_bridge_query_classify(db.conn, bad_sql, sizeof(bad_sql) - 1, &info, &error) == PDD_BRIDGE_ERROR,
			"Embedded NUL SQL was truncated");
		pdd_bridge_error_free(&error);
		require(scalar(observer, "SELECT count(*) FROM bridge_query_t") == 0, "NUL SQL executed a suffix");
	} catch (...) {
		duckdb_disconnect(&observer);
		throw;
	}
	duckdb_disconnect(&observer);
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
void legacy_parser_test(const char *extension_directory) {
	Database native, bridge;
	for (auto conn : {native.conn, bridge.conn}) {
		query(conn, "SET extension_directory=" + sql_quote(extension_directory));
		query(conn, "LOAD legacy_parser");
		query(conn, "CREATE TABLE bridge_legacy(i INTEGER PRIMARY KEY,s VARCHAR)");
	}
	for (bool enabled : {false, true}) {
		for (const char *mode : {"DEFAULT", "FALLBACK", "STRICT"}) {
			for (auto conn : {native.conn, bridge.conn}) {
				query(conn, std::string("SET enable_legacy_parser=") + (enabled ? "true" : "false"));
				query(conn, "SET allow_parser_override_extension=" + sql_quote(mode));
			}
			bool legacy = !enabled;
			pdd_bridge_error error = {};
			checked(pdd_bridge_connection_uses_legacy_parser(bridge.conn, &legacy, &error), error);
			require(legacy == (enabled && std::strcmp(mode, "DEFAULT") != 0), "Effective legacy parser setting is wrong");
			for (bool backtick : {false, true}) {
				std::string sql = std::string("SELECT $1_0") + (backtick ? " AS `v`" : "") + ", $1,$2,$3,$4,$5,$6,$7,$8,$9";
				bool selected = false;
				checked(pdd_bridge_query_uses_legacy_parser(bridge.conn, sql.data(), sql.size(), &selected, &error), error);
				bool expected = legacy && (!backtick || std::strcmp(mode, "STRICT") == 0);
				require(selected == expected, "Query-specific parser probe chose the wrong numeric grammar");
				duckdb_prepared_statement prepared = nullptr;
				auto status = duckdb_prepare(native.conn, sql.c_str(), &prepared);
				if (backtick && legacy && std::strcmp(mode, "STRICT") == 0) {
					require(status == DuckDBError, "Strict legacy parser accepted the PEG-only numeric fixture");
				} else {
					require(status == DuckDBSuccess, "Native numeric parser fixture failed");
					auto count = duckdb_nparams(prepared);
					require(count == (expected ? 9 : 10), "Native parameter count disagrees with selected grammar");
					for (idx_t parameter = 1; parameter <= count; ++parameter) {
						require(duckdb_bind_int32(prepared, parameter, static_cast<int32_t>(parameter)) == DuckDBSuccess,
							"Native numeric fixture binding failed");
					}
					duckdb_result result = {};
					require(duckdb_execute_prepared(prepared, &result) == DuckDBSuccess, "Native numeric fixture execution failed");
					require(duckdb_value_int32(&result, 0, 0) == (expected ? 1 : 10), "Selected grammar changed the numeric parameter value");
					duckdb_destroy_result(&result);
				}
				duckdb_destroy_prepare(&prepared);
			}
			query(bridge.conn, "BEGIN");
			query(bridge.conn, "INSERT INTO bridge_legacy VALUES(17,'pending')");
			for (const char *probe : {"SELECT 1 FROM", "SELECT '\xff'", "", "INSERT INTO bridge_legacy VALUES(999,'must-not-execute')"}) {
				bool selected = false;
				checked(pdd_bridge_query_uses_legacy_parser(bridge.conn, probe, std::strlen(probe), &selected, &error), error);
			}
			query(bridge.conn, "INSERT INTO bridge_legacy VALUES(18,'still-valid')");
			query(bridge.conn, "COMMIT");
			require(scalar(bridge.conn, "SELECT count(*) FROM bridge_legacy") == 2 &&
				scalar(bridge.conn, "SELECT sum(i) FROM bridge_legacy") == 35,
				"Parser mode probe executed SQL or changed the actual transaction");
			require(scalar_string(bridge.conn, "SELECT current_setting('allow_parser_override_extension')") == mode,
				"Parser mode probe changed the actual override setting");
			query(bridge.conn, "DELETE FROM bridge_legacy");
			diagnostic_test(bridge.conn);
			for (const char *sql : {
				"INSERT INTO bridge_legacy VALUES(1,'one'); INSERT INTO bridge_legacy VALUES(2,'two')",
				"SELECT 42; INSERT INTO bridge_legacy VALUES(3,'selected')",
				"BEGIN; INSERT INTO bridge_legacy VALUES(4,'transaction'); COMMIT",
				"/* outer /* nested ; */ */ INSERT INTO bridge_legacy VALUES(5,'five'); -- next ;\n INSERT INTO bridge_legacy VALUES(6,'six')",
				"INSERT INTO bridge_legacy VALUES(7,'seven'); /* INSERT INTO bridge_legacy VALUES(8,'eight') */ INSERT INTO bridge_legacy VALUES(8,'eight')",
				"INSERT INTO bridge_legacy VALUES(9,$tag$dollar; 'quoted'$tag$); UPDATE bridge_legacy SET s=s||';tail'",
				"INSERT\xc2\xa0INTO bridge_legacy VALUES(10,'unicode'); SELECT 1; SELECT 1",
				";; INSERT INTO bridge_legacy VALUES(11,'eleven'); ; INSERT INTO bridge_legacy VALUES(12,'twelve');;",
				"INSERT INTO bridge_legacy VALUES(13,'prefix'); SELEC invalid",
				"INSERT INTO bridge_legacy VALUES(14,'prefix'); SELECT 'unterminated",
				"INSERT INTO bridge_legacy VALUES(15,'prefix'); SELECT /* unterminated"}) {
				query(native.conn, "DELETE FROM bridge_legacy");
				query(bridge.conn, "DELETE FROM bridge_legacy");
				duckdb_result result = {};
				auto native_status = duckdb_query(native.conn, sql, &result);
				auto native_rows = duckdb_rows_changed(&result);
				duckdb_destroy_result(&result);
				pdd_bridge_exec_info progress = {};
				auto status = pdd_bridge_exec_batch(bridge.conn, sql, std::strlen(sql), false, &progress, &error);
				pdd_bridge_error_free(&error);
				require((status == PDD_BRIDGE_OK) == (native_status == DuckDBSuccess), "Legacy batch status differs from native query");
				if (status == PDD_BRIDGE_OK) {
					require(progress.rows_changed == native_rows, "Legacy batch affected rows differ from native query");
				}
				const char snapshot[] = "SELECT coalesce(string_agg(i::VARCHAR||':'||s,'|' ORDER BY i),'') FROM bridge_legacy";
				require(scalar_string(bridge.conn, snapshot) == scalar_string(native.conn, snapshot),
					"Legacy batch changed committed-prefix data or replayed a statement");
			}
			query(bridge.conn, "DELETE FROM bridge_legacy");
			batch_counts(bridge.conn, "BEGIN; INSERT INTO bridge_legacy VALUES(1,'one'); INSERT INTO bridge_legacy VALUES(2,'two'); "
				"SELECT 42; INSERT INTO bridge_legacy VALUES(3,'three')", 1, 5, true, PDD_BRIDGE_TRANSACTION_BEGIN);
			query(bridge.conn, "COMMIT");
			require(scalar(bridge.conn, "SELECT count(*) FROM bridge_legacy") == 3, "Forced legacy batch lost committed writes");
			query(bridge.conn, "DELETE FROM bridge_legacy");
			auto failed = batch(bridge.conn, "BEGIN; INSERT INTO bridge_legacy VALUES(4,'must-not-execute'); SELEC invalid", true, true);
			require(failed.parse_failed && failed.completed_statements == 0 &&
				scalar(bridge.conn, "SELECT count(*) FROM bridge_legacy") == 0, "Forced legacy batch executed before complete parsing");
		}
	}
}

int unused_port() {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	require(fd >= 0, "Socket failed");
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
		close(fd);
		throw std::runtime_error("Loopback bind failed");
	}
	socklen_t size = sizeof(address);
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size)) {
		close(fd);
		throw std::runtime_error("getsockname failed");
	}
	close(fd);
	return ntohs(address.sin_port);
}

void remote_test(const char *extension_directory) {
	Database server, client;
	for (auto conn : {server.conn, client.conn}) {
		query(conn, "SET extension_directory=" + sql_quote(extension_directory));
		query(conn, "LOAD httpfs");
		query(conn, "LOAD quack");
	}
	query(server.conn, "CREATE TABLE bridge_query_t(i INTEGER PRIMARY KEY,s VARCHAR,b BLOB)");
	std::string endpoint = "quack:127.0.0.1:" + std::to_string(unused_port());
	query(server.conn, "CALL quack_serve(" + sql_quote(endpoint) + ",token='bridge-test',disable_ssl=true)");
	try {
		std::string attach = "ATTACH " + sql_quote(endpoint) + " AS q (TOKEN 'bridge-test',DISABLE_SSL true)";
		batch_counts(client.conn, "ATTACH " + sql_quote(endpoint) + " AS bridge_batch_remote (TOKEN 'bridge-test',DISABLE_SSL true); "
			"CONNECT bridge_batch_remote; INSERT INTO bridge_query_t VALUES(77,'batch-routing',NULL); "
			"DISCONNECT; DETACH bridge_batch_remote", 0, 5);
		require(scalar(server.conn, "SELECT count(*) FROM bridge_query_t WHERE i=77") == 1,
			"ATTACH/CONNECT batch duplicated or lost its remote insert");
		query(server.conn, "DELETE FROM bridge_query_t");
		auto phase = batch(client.conn, "ATTACH " + sql_quote(endpoint) + " AS bridge_phase_remote (TOKEN 'bridge-test',DISABLE_SSL true); "
			"CONNECT bridge_phase_remote; INSERT INTO bridge_query_t VALUES(80,'force-routing',NULL); "
			"DISCONNECT; DETACH bridge_phase_remote; SELECT * FROM bridge_phase_missing", true, true);
		require(phase.completed_statements == 5 && !phase.parse_failed && phase.prepare_failed &&
			scalar(server.conn, "SELECT count(*) FROM bridge_query_t WHERE i=80") == 1,
			"Forced batch failed to switch safely between local preparation and remote execution");
		query(server.conn, "DELETE FROM bridge_query_t");
		check_remote_catalogs(client.conn, false);
		{
			Result begin(client.conn, "BEGIN");
			Result attached(client.conn, attach);
			check_remote_catalogs(client.conn, true);
			Result rollback(client.conn, "ROLLBACK");
			check_remote_catalogs(client.conn, false);
		}
		{
			Result attached(client.conn, attach);
		}
		check_remote_catalogs(client.conn, true);
		batch_counts(client.conn, "INSERT INTO q.bridge_query_t VALUES(78,'qualified-batch',NULL); "
			"UPDATE q.bridge_query_t SET i=79 WHERE i=78", 1, 2);
		require(scalar(server.conn, "SELECT count(*) FROM bridge_query_t WHERE i=79") == 1,
			"Qualified attached-catalog batch lost or duplicated its write");
		query(server.conn, "DELETE FROM bridge_query_t WHERE i=79");
		common_query_test(client.conn, server.conn, "q.bridge_query_t");
		for (const char *sql : {"SELECT 1 FROM", "", "SELECT 1; SELECT 2"}) {
			classify_transaction_test(client.conn, server.conn, "q.bridge_query_t", false, sql);
		}
		check_remote(client.conn, false);
		TemporaryDirectory local_writers;
		configure_writers(client.conn, local_writers);
		{
			Result connected(client.conn, "CONNECT q");
		}
		check_remote(client.conn, true);
		common_query_test(client.conn, server.conn, "bridge_query_t");
		classify_transaction_test(client.conn, server.conn, "bridge_query_t", true);
		batch_test(client.conn, server.conn);
		other_affected_rows_test(client.conn);
		writer_test(client.conn, server.conn, "bridge_query_t", &local_writers);
		stream_test(client.conn);
		{
			Result disconnected(client.conn, "DISCONNECT");
		}
		check_remote(client.conn, false);
		{
			Result detached(client.conn, "DETACH q");
		}
		check_remote_catalogs(client.conn, false);
	} catch (...) {
		query(server.conn, "SELECT * FROM quack_stop(" + sql_quote(endpoint) + ")");
		throw;
	}
	query(server.conn, "SELECT * FROM quack_stop(" + sql_quote(endpoint) + ")");
}
#endif

} // namespace

int main(int argc, char **argv) {
	try {
		local_test();
		std::puts("PASS: local classification, errors, data, transactions, affected rows, and lazy streaming");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
		require(argc == 2, "Pass the directory containing GA quack, httpfs, and legacy_parser extensions");
		legacy_parser_test(argv[1]);
		std::puts("PASS: default and legacy parser settings, batch boundaries, prefixes, counts, and diagnostics");
		remote_test(argv[1]);
		std::puts("PASS: ATTACH and CONNECT classification, execution, bytes, transactions, rows, and streaming");
#else
		(void) argc;
		(void) argv;
#endif
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
