#include "pdo_duckdb_bridge_logging.h"
#include "pdo_duckdb_bridge_query.h"
#include "pdo_duckdb_bridge_build.h"
#include "duckdb/main/connection.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __linux__
namespace {
thread_local bool fail_next_allocation = false;
thread_local bool allocation_failure_seen = false;
}

void *operator new(std::size_t size) {
	if (fail_next_allocation) {
		fail_next_allocation = false;
		allocation_failure_seen = true;
		throw std::bad_alloc();
	}
	if (void *memory = std::malloc(size ? size : 1)) {
		return memory;
	}
	throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
	return ::operator new(size);
}

void operator delete(void *memory) noexcept {
	std::free(memory);
}

void operator delete[](void *memory) noexcept {
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
	std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
	std::free(memory);
}
#endif

namespace {

void require(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void checked(pdd_bridge_status status, pdd_bridge_error &error) {
	std::string message = error.data ? std::string(error.data, error.length) : "Bridge error without message";
	pdd_bridge_error_free(&error);
	if (status != PDD_BRIDGE_OK) {
		throw std::runtime_error(message);
	}
}

struct Directory {
	std::filesystem::path path;
	Directory() {
		std::random_device random;
		for (int attempt = 0; attempt < 10; attempt++) {
			path = std::filesystem::temp_directory_path() / ("pdo-duckdb-global-logging-" + std::to_string(random()));
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
	duckdb_database db = nullptr;
	duckdb_connection conn = nullptr;
	duckdb_connection observer = nullptr;
	Database() {
		require(duckdb_open(nullptr, &db) == DuckDBSuccess, "Open failed");
		require(duckdb_connect(db, &conn) == DuckDBSuccess, "Connect failed");
		require(duckdb_connect(db, &observer) == DuckDBSuccess, "Observer connect failed");
	}
	~Database() {
		duckdb_disconnect(&observer);
		duckdb_disconnect(&conn);
		duckdb_close(&db);
	}
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
};

struct NativeResult {
	duckdb_result result = {};
	NativeResult(duckdb_connection connection, const std::string &sql) {
		if (duckdb_query(connection, sql.c_str(), &result) != DuckDBSuccess) {
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

void query(duckdb_connection connection, const std::string &sql) {
	NativeResult result(connection, sql);
}

int64_t scalar(duckdb_connection connection, const std::string &sql) {
	NativeResult result(connection, sql);
	require(duckdb_row_count(&result.result) == 1, "Expected scalar row");
	return duckdb_value_int64(&result.result, 0, 0);
}

std::string quote(const std::string &text) {
	std::string result = "'";
	for (char c : text) {
		result += c;
		if (c == '\'') {
			result += c;
		}
	}
	return result + "'";
}

using Files = std::map<std::string, std::string>;

Files snapshot(const std::filesystem::path &directory) {
	Files files;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(directory)) {
		if (entry.is_regular_file()) {
			std::ifstream input(entry.path(), std::ios::binary);
			files.emplace(entry.path().string(),
				std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
		}
	}
	return files;
}

bool contains(const Files &files, const std::string &marker) {
	for (auto &entry : files) {
		if (entry.second.find(marker) != std::string::npos) {
			return true;
		}
	}
	return false;
}

duckdb::LogManager &log_manager(duckdb_connection connection) {
	return duckdb::LogManager::Get(*reinterpret_cast<duckdb::Connection *>(connection)->context);
}

template <typename Operation>
void require_revoked(Operation operation, const std::filesystem::path &directory, const Files &before) {
	std::string message;
	try {
		operation();
	} catch (const std::exception &error) {
		message = error.what();
	}
	require(snapshot(directory) == before, "Retained file storage wrote after containment returned");
	require(message.find("path wasn't set") != std::string::npos,
		"Retained file storage did not report a revoked path");
}

void retained_storage_operations(duckdb::LogStorage &storage, duckdb_connection connection,
	const std::filesystem::path &directory, const Files &before, bool normalized) {
	require_revoked([&] { storage.FlushAll(); }, directory, before);
	duckdb::vector<duckdb::Value> inputs;
	duckdb::vector<duckdb::LogicalType> types;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	duckdb::named_argument_map_t parameters;
	duckdb::vector<duckdb::Identifier> names;
	duckdb::BoundTableFunction function;
#else
	duckdb::named_parameter_map_t parameters;
	duckdb::vector<duckdb::string> names;
	duckdb::TableFunction function;
#endif
	duckdb::TableFunctionRef reference;
	duckdb::TableFunctionBindInput input(inputs, parameters, types, names, nullptr, nullptr, function, reference);
	auto &context = *reinterpret_cast<duckdb::Connection *>(connection)->context;
	auto target = normalized ? duckdb::LoggingTargetTable::LOG_ENTRIES : duckdb::LoggingTargetTable::ALL_LOGS;
	require_revoked([&] { storage.BindReplace(context, input, target); }, directory, before);
}

class BufferingStorageMembers : public duckdb::BufferingLogStorage {
public:
	static auto Mutex() noexcept {
		return &BufferingStorageMembers::lock;
	}
};

class LogManagerConfigMembers : public duckdb::LogManager {
public:
	static auto Mutex() noexcept {
		return &LogManagerConfigMembers::lock;
	}
	static auto Config() noexcept {
		return &LogManagerConfigMembers::config;
	}
};

struct Stream {
	pdd_bridge_result *result = nullptr;
	idx_t seen = 0;
	explicit Stream(duckdb_connection connection) {
		const std::string sql = "SELECT i AS armed_stream FROM range(100000) t(i)";
		pdd_bridge_error error = {};
		checked(pdd_bridge_query_execute(connection, sql.data(), sql.size(), true, &result, &error), error);
		idx_t columns, rows_changed;
		bool streaming = false;
		checked(pdd_bridge_result_info(result, &columns, &rows_changed, &streaming, &error), error);
		require(streaming && columns == 1 && rows_changed == 0, "Expected a live SELECT stream");
	}
	~Stream() {
		pdd_bridge_result_destroy(&result);
	}
	Stream(const Stream &) = delete;
	Stream &operator=(const Stream &) = delete;
	idx_t next() {
		duckdb_data_chunk chunk = nullptr;
		pdd_bridge_error error = {};
		checked(pdd_bridge_result_fetch(result, &chunk, &error), error);
		if (!chunk) {
			return 0;
		}
		auto rows = duckdb_data_chunk_get_size(chunk);
		auto *values = static_cast<int64_t *>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)));
		for (idx_t i = 0; i < rows; ++i) {
			if (values[i] != static_cast<int64_t>(seen + i)) {
				duckdb_destroy_data_chunk(&chunk);
				throw std::runtime_error("Containment changed a streamed value");
			}
		}
		seen += rows;
		duckdb_destroy_data_chunk(&chunk);
		return rows;
	}
};

void local_test(bool locked, bool live) {
	Directory directory;
	Files before;
	{
		Database database;
		query(database.conn, "SET threads=1");
		query(database.conn, "CREATE TABLE writes(i INTEGER)");
		query(database.conn, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.path.string()) + ")");
		query(database.conn, "SELECT 'query_log_positive_control'");
		require(contains(snapshot(directory.path), "query_log_positive_control"), "File logging positive control did not write");
		if (locked) {
			query(database.conn, "SET lock_configuration=true");
		}
		query(database.conn, "BEGIN");
		query(database.conn, "INSERT INTO writes VALUES(17)");
		if (live) {
			Stream stream(database.conn);
			idx_t rows = stream.next();
			require(rows > 0, "Stream returned no first chunk");
			before = snapshot(directory.path);
			pdd_bridge_error error = {};
			checked(pdd_bridge_clear_instance_writers(database.conn, &error), error);
			require(snapshot(directory.path) == before, "Containment flushed bytes to the old file sink");
			idx_t count;
			while ((count = stream.next()) != 0) {
				rows += count;
			}
			require(rows == 100000, "Containment disrupted the live stream");
		} else {
			before = snapshot(directory.path);
			pdd_bridge_error error = {};
			checked(pdd_bridge_clear_instance_writers(database.conn, &error), error);
			require(snapshot(directory.path) == before, "Containment flushed bytes to the old file sink");
		}
		query(database.observer, "SELECT count(*) FROM duckdb_extensions()");
		require(scalar(database.observer, "SELECT count(*) FROM writes") == 0, "Containment committed the transaction");
		query(database.conn, "COMMIT");
		require(scalar(database.observer, "SELECT i FROM writes") == 17, "Containment lost the pending write");
		require(snapshot(directory.path) == before, "Later SQL wrote to the old file sink");
	}
	require(snapshot(directory.path) == before, "Database destruction wrote to the old file sink");
}

void memory_test(const char *storage) {
	Database database;
	query(database.conn, "CALL enable_logging('QueryLog', storage=" + quote(storage) + ")");
	query(database.conn, "SELECT 17 AS memory_retention_probe");
	const std::string count = "SELECT count(*) FROM duckdb_logs() WHERE message='SELECT 17 AS memory_retention_probe'";
	require(scalar(database.observer, count) == 1, "Memory logging positive control did not write");
	pdd_bridge_error error = {};
	checked(pdd_bridge_clear_instance_writers(database.conn, &error), error);
	require(scalar(database.observer, count) == 1, "Containment discarded existing memory log records");
	require(scalar(database.observer, "SELECT current_setting('enable_logging')::INTEGER") == 1,
		"Containment disabled safe memory logging");
	query(database.conn, "SELECT 18 AS memory_continues_probe");
	require(scalar(database.observer,
		"SELECT count(*) FROM duckdb_logs() WHERE message='SELECT 18 AS memory_continues_probe'") == 1,
		"Memory logging stopped after containment");
}

void retained_logger_test() {
	Directory directory;
	Files before;
	{
		Database database;
		query(database.conn, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.path.string()) + ")");
		auto &context = *reinterpret_cast<duckdb::Connection *>(database.conn)->context;
		auto logger = duckdb::LogManager::Get(context).GlobalLoggerReference();
		require(logger->IsThreadSafe(), "Retained logger must support concurrent callers");
		logger->WriteLog("", duckdb::LogLevel::LOG_INFO, "retained_logger_positive_control");
		logger->Flush();
		before = snapshot(directory.path);
		require(contains(before, "retained_logger_positive_control"), "Retained logger positive control did not write");
		pdd_bridge_error error = {};
		checked(pdd_bridge_clear_instance_writers(database.conn, &error), error);
		std::exception_ptr failure;
		std::thread writer([&] {
			try {
				for (int i = 0; i < 10000; ++i) {
					logger->WriteLog("", duckdb::LogLevel::LOG_INFO, "retained_logger_after_containment");
				}
				logger->Flush();
			} catch (...) {
				failure = std::current_exception();
			}
		});
		writer.join();
		if (failure) {
			std::rethrow_exception(failure);
		}
		require(scalar(database.observer,
			"SELECT count(*) FROM duckdb_logs() WHERE type='' AND message='retained_logger_after_containment'") == 10000,
			"Retained logger did not route every message to memory");
		require(snapshot(directory.path) == before, "Retained logger wrote to the old file sink");
	}
	require(snapshot(directory.path) == before, "Retained logger teardown wrote to the old file sink");
}

void storage_mismatch_test(bool actual_file) {
	Directory directory;
	Files before;
	{
		Database database;
		query(database.conn, actual_file ?
			"CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.path.string()) + ")" :
			"CALL enable_logging('QueryLog', storage='memory')");
		query(database.conn, "SELECT 'storage_mismatch_positive_control'");
		auto &manager = log_manager(database.conn);
		manager.SetEnableLogging(false);
		auto storage = manager.GetLogStorage();
		if (actual_file) {
			manager.GlobalLoggerReference()->WriteLog("", duckdb::LogLevel::LOG_INFO, "storage_mismatch_pending_record");
		}
		before = snapshot(directory.path);
		if (actual_file) {
			require(contains(before, "storage_mismatch_positive_control"), "Storage mismatch positive control did not write");
			require(!contains(before, "storage_mismatch_pending_record"), "Storage mismatch record was not buffered");
		}
		{
			std::lock_guard<duckdb::mutex> lock(manager.*LogManagerConfigMembers::Mutex());
			(manager.*LogManagerConfigMembers::Config()).storage = actual_file ? "memory" : "file";
		}
		pdd_bridge_error error = {};
		auto status = pdd_bridge_clear_instance_writers(database.conn, &error);
		std::string message = error.data ? std::string(error.data, error.length) : "";
		pdd_bridge_error_free(&error);
		require(status == PDD_BRIDGE_ERROR && message.find("Inconsistent DuckDB log storage configuration") != std::string::npos,
			"Injected storage mismatch was not reported");
		require(snapshot(directory.path) == before, "Storage mismatch flushed the old file sink");
		if (actual_file) {
			retained_storage_operations(*storage, database.observer, directory.path, before, true);
		}
		query(database.conn, "SELECT 42");
	}
	require(snapshot(directory.path) == before, "Storage mismatch teardown wrote to the old file sink");
}

void retained_storage_test(bool normalized) {
	Directory directory;
	Files before;
	{
		Database database;
		auto path = normalized ? directory.path : directory.path / "all_logs.csv";
		query(database.conn, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(path.string()) +
			(normalized ? ")" : ", storage_normalize=false)"));
		query(database.conn, "SELECT 'retained_storage_positive_control'");
		auto &manager = log_manager(database.conn);
		manager.SetEnableLogging(false);
		auto storage = manager.GetLogStorage();
		require(dynamic_cast<duckdb::FileLogStorage *>(storage.get()) != nullptr, "Expected engine file log storage");
		manager.GlobalLoggerReference()->WriteLog("", duckdb::LogLevel::LOG_INFO, "retained_storage_pending_record");
		before = snapshot(directory.path);
		require(contains(before, "retained_storage_positive_control"), "Retained storage positive control did not write");
		require(!contains(before, "retained_storage_pending_record"), "Retained storage record was not buffered");
		pdd_bridge_error error = {};
		checked(pdd_bridge_clear_instance_writers(database.conn, &error), error);
		require(snapshot(directory.path) == before, "Containment flushed retained storage");
		retained_storage_operations(*storage, database.observer, directory.path, before, normalized);
	}
	require(snapshot(directory.path) == before, "Retained storage destruction wrote after containment");
}

Files concurrent_binder(duckdb_connection owner, duckdb_connection reader, const std::filesystem::path &directory,
	const std::function<void()> &prepare_reader = {}) {
	Files after_helper;
	{
		query(owner, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.string()) + ")");
		query(owner, "SELECT 'concurrent_binder_positive_control'");
		auto &manager = log_manager(owner);
		manager.SetEnableLogging(false);
		if (prepare_reader) {
			prepare_reader();
		}
		manager.GlobalLoggerReference()->WriteLog("", duckdb::LogLevel::LOG_INFO, "concurrent_binder_pending_record");
		auto storage = manager.GetLogStorage();
		auto &buffering = dynamic_cast<duckdb::BufferingLogStorage &>(*storage);
		std::unique_lock<duckdb::mutex> storage_lock(buffering.*BufferingStorageMembers::Mutex());
		const auto before = snapshot(directory);
		require(contains(before, "concurrent_binder_positive_control"), "Concurrent binder positive control did not write");
		require(!contains(before, "concurrent_binder_pending_record"), "Concurrent binder record was not buffered");
		const auto references = storage.use_count();
		std::string binder_error;
		std::thread binder([&] {
			try {
				query(reader, "SELECT * FROM duckdb_logs()");
			} catch (const std::exception &error) {
				binder_error = error.what();
			}
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (storage.use_count() == references && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		if (storage.use_count() == references) {
			storage_lock.unlock();
			binder.join();
			throw std::runtime_error("Real duckdb_logs binder did not retain the old storage: " +
				(binder_error.empty() ? std::string("query completed") : binder_error));
		}
		std::atomic<bool> helper_started {false};
		std::exception_ptr helper_failure;
		try {
			std::thread helper([&] {
				helper_started.store(true);
				try {
					pdd_bridge_error error = {};
					checked(pdd_bridge_clear_instance_writers(owner, &error), error);
					after_helper = snapshot(directory);
				} catch (...) {
					helper_failure = std::current_exception();
				}
			});
			while (!helper_started.load()) {
				std::this_thread::yield();
			}
			storage_lock.unlock();
			helper.join();
		} catch (...) {
			if (storage_lock.owns_lock()) {
				storage_lock.unlock();
			}
			binder.join();
			throw;
		}
		binder.join();
		if (helper_failure) {
			std::rethrow_exception(helper_failure);
		}
		require(binder_error.empty() || binder_error.find("path wasn't set") != std::string::npos,
			"Concurrent binder failed for a reason other than revoked storage");
		require(snapshot(directory) == after_helper, "Concurrent binder wrote after containment returned");
		retained_storage_operations(*storage, owner, directory, after_helper, true);
	}
	return after_helper;
}

void concurrent_binder_test() {
	Directory directory;
	Files after_helper;
	{
		Database database;
		after_helper = concurrent_binder(database.conn, database.observer, directory.path);
	}
	require(snapshot(directory.path) == after_helper, "Concurrent binder teardown wrote after containment");
}

#ifdef __linux__
void allocation_failure_test() {
	Directory directory;
	Files before;
	{
		Database database;
		query(database.conn, "SET threads=1");
		query(database.conn, "CREATE TABLE writes(i INTEGER)");
		query(database.conn, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.path.string()) + ")");
		query(database.conn, "SELECT 'allocation_failure_positive_control'");
		require(contains(snapshot(directory.path), "allocation_failure_positive_control"), "Allocation failure positive control did not write");
		query(database.conn, "BEGIN");
		query(database.conn, "INSERT INTO writes VALUES(17)");
		auto storage = log_manager(database.conn).GetLogStorage();
		{
			Stream stream(database.conn);
			idx_t rows = stream.next();
			require(rows > 0, "Allocation failure stream returned no first chunk");
			before = snapshot(directory.path);
			pdd_bridge_error error = {};
			allocation_failure_seen = false;
			fail_next_allocation = true;
			auto status = pdd_bridge_clear_instance_writers(database.conn, &error);
			fail_next_allocation = false;
			bool has_error = error.data != nullptr && error.length > 0;
			pdd_bridge_error_free(&error);
			require(allocation_failure_seen, "Allocation failure was not exercised");
			require(status == PDD_BRIDGE_ERROR, "Allocation failure was not reported");
			require(has_error, "Allocation failure omitted its error diagnostic");
			require(snapshot(directory.path) == before, "Allocation failure flushed the old file sink");
			retained_storage_operations(*storage, database.observer, directory.path, before, true);
			idx_t count;
			while ((count = stream.next()) != 0) {
				rows += count;
			}
			require(rows == 100000, "Allocation failure disrupted the live stream");
		}
		require(scalar(database.observer, "SELECT count(*) FROM writes") == 0, "Allocation failure committed the transaction");
		query(database.conn, "COMMIT");
		require(scalar(database.observer, "SELECT i FROM writes") == 17, "Allocation failure lost the pending write");
		require(snapshot(directory.path) == before, "Later SQL after allocation failure wrote to the old file sink");
	}
	require(snapshot(directory.path) == before, "Allocation failure teardown wrote to the old file sink");
}
#endif

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
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

idx_t connection_id(duckdb_connection connection) {
	duckdb_client_context context = nullptr;
	duckdb_connection_get_client_context(connection, &context);
	require(context != nullptr, "Client context is missing");
	auto id = duckdb_client_context_get_connection_id(context);
	duckdb_destroy_client_context(&context);
	return id;
}

void remote_test(const char *extension_directory) {
	Directory directory;
	Directory server_directory;
	Files before;
	Files server_after;
	{
		Database server, client;
		for (auto conn : {server.conn, client.conn}) {
			query(conn, "SET threads=1");
			query(conn, "SET extension_directory=" + quote(extension_directory));
			query(conn, "LOAD httpfs");
			query(conn, "LOAD quack");
		}
		query(server.conn, "CREATE TABLE writes(i INTEGER)");
		query(server.conn, "CALL enable_logging('QueryLog', storage='memory')");
		const std::string remote_count = "SELECT count(*) FROM duckdb_logs() l JOIN duckdb_log_contexts() c USING(context_id) "
			"WHERE l.type='QueryLog' AND c.connection_id NOT IN (" + std::to_string(connection_id(server.conn)) + "," +
			std::to_string(connection_id(server.observer)) + ")";
		std::string endpoint = "quack:127.0.0.1:" + std::to_string(unused_port());
		query(server.conn, "CALL quack_serve(" + quote(endpoint) + ",token='bridge-test',disable_ssl=true)");
		try {
			query(client.conn, "CALL enable_logging('QueryLog', storage='file', storage_path=" + quote(directory.path.string()) + ")");
			query(client.conn, "SELECT 'remote_local_log_positive_control'");
			require(contains(snapshot(directory.path), "remote_local_log_positive_control"), "Remote file logging positive control did not write");
			query(client.conn, "ATTACH " + quote(endpoint) + " AS q (TOKEN 'bridge-test',DISABLE_SSL true)");
			query(client.conn, "CONNECT q");
			query(client.conn, "BEGIN");
			query(client.conn, "INSERT INTO writes VALUES(17)");
			{
				Stream stream(client.conn);
				idx_t rows = stream.next();
				require(rows > 0, "Remote stream returned no first chunk");
				before = snapshot(directory.path);
				auto remote_before = scalar(server.observer, remote_count);
				require(remote_before > 0, "Remote QueryLog positive control did not write");
				pdd_bridge_error error = {};
				checked(pdd_bridge_clear_instance_writers(client.conn, &error), error);
				require(scalar(server.observer, remote_count) == remote_before, "Containment dispatched remote SQL");
				require(scalar(server.observer, "SELECT current_setting('enable_logging')::INTEGER") == 1,
					"Containment disabled the remote server logger");
				require(snapshot(directory.path) == before, "Remote containment flushed the old local file sink");
				idx_t count;
				while ((count = stream.next()) != 0) {
					rows += count;
				}
				require(rows == 100000, "Remote containment disrupted the live stream");
			}
			require(scalar(server.observer, "SELECT count(*) FROM writes") == 0, "Remote containment committed the transaction");
			query(client.conn, "COMMIT");
			require(scalar(server.observer, "SELECT i FROM writes") == 17, "Remote containment lost the pending write");
			query(client.conn, "DISCONNECT");
			query(client.conn, "DETACH q");
			require(snapshot(directory.path) == before, "Later SQL wrote to the old local file sink");
			server_after = concurrent_binder(server.conn, client.conn, server_directory.path, [&] {
				query(client.conn, "ATTACH " + quote(endpoint) + " AS q (TOKEN 'bridge-test',DISABLE_SSL true)");
				query(client.conn, "CONNECT q");
			});
			query(client.conn, "DISCONNECT");
			query(client.conn, "DETACH q");
		} catch (...) {
			query(server.conn, "SELECT * FROM quack_stop(" + quote(endpoint) + ")");
			throw;
		}
		query(server.conn, "SELECT * FROM quack_stop(" + quote(endpoint) + ")");
	}
	require(snapshot(directory.path) == before, "Remote teardown wrote to the old local file sink");
	require(snapshot(server_directory.path) == server_after, "Quack binder teardown wrote to the old server file sink");
}
#endif

} // namespace

int main(int argc, char **argv) {
	try {
		for (bool live : {false, true}) {
			for (bool locked : {false, true}) {
				try {
					local_test(locked, live);
				} catch (const std::exception &error) {
					throw std::runtime_error(std::string("locked=") + (locked ? "true" : "false") +
						" live=" + (live ? "true: " : "false: ") + error.what());
				}
			}
		}
		for (const auto *storage : {"memory", "MEMORY", "MemOrY"}) {
			memory_test(storage);
		}
		retained_logger_test();
		std::puts("PASS: existing memory logs survive and a retained logger sends 10000 subsequent messages to memory");
		retained_storage_test(true);
		retained_storage_test(false);
		concurrent_binder_test();
		storage_mismatch_test(true);
		storage_mismatch_test(false);
		std::puts("PASS: retained file storage and concurrent duckdb_logs binding cannot write after containment returns");
#ifdef __linux__
		allocation_failure_test();
		std::puts("PASS: allocation failure leaves the old file sink detached through stream consumption, commit, and destruction");
#endif
		std::puts("PASS: global log files stop before inspector SQL and teardown, with locked config and live transactions/results");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 && !defined(_WIN32)
		require(argc == 2, "Pass the directory containing GA quack and httpfs extensions");
		remote_test(argv[1]);
		std::puts("PASS: CONNECT containment emits no remote SQL and preserves the live stream and transaction");
		std::puts("PASS: concurrent quack listener log binding cannot write to revoked server file storage");
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
