#include "pdo_duckdb_bridge_filesystem.h"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

#include <chrono>
#include <condition_variable>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void checked(pdd_bridge_status status, pdd_bridge_error &error) {
	std::string message = error.data ? std::string(error.data, error.length) : "Bridge returned no error message";
	pdd_bridge_error_free(&error);
	if (status != PDD_BRIDGE_OK) {
		throw std::runtime_error(message);
	}
}

std::string quote(const std::filesystem::path &path) {
	std::string result = "'";
	for (char byte : path.u8string()) {
		result += byte;
		if (byte == '\'') {
			result += byte;
		}
	}
	return result + "'";
}

struct Directory {
	std::filesystem::path path;
	Directory() {
		std::random_device random;
		for (unsigned attempt = 0; attempt < 10; attempt++) {
			path = std::filesystem::temp_directory_path() / ("pdo-duckdb-filesystem-" + std::to_string(random()));
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

struct Cache {
	duckdb_instance_cache handle = duckdb_create_instance_cache();
	Cache() { require(handle != nullptr, "Cannot create instance cache"); }
	~Cache() { duckdb_destroy_instance_cache(&handle); }
	Cache(const Cache &) = delete;
	Cache &operator=(const Cache &) = delete;
};

struct Config {
	duckdb_config handle = nullptr;
	Config() { require(duckdb_create_config(&handle) == DuckDBSuccess, "Cannot create config"); }
	~Config() { duckdb_destroy_config(&handle); }
	Config(const Config &) = delete;
	Config &operator=(const Config &) = delete;
};

struct Database {
	duckdb_database handle = nullptr;
	duckdb_connection connection = nullptr;
	Database(Cache &cache, const std::filesystem::path &path, const char *access_mode = nullptr) {
		try {
			Config config;
			if (access_mode) {
				require(duckdb_set_config(config.handle, "access_mode", access_mode) == DuckDBSuccess,
						"Cannot set access mode");
			}
			pdd_bridge_error error = {};
			checked(pdd_bridge_filesystem_install(config.handle, &error), error);
			checked(pdd_bridge_filesystem_install(config.handle, &error), error);
			char *open_error = nullptr;
			auto status = duckdb_get_or_create_from_cache(cache.handle, path.u8string().c_str(), &handle, config.handle,
														  &open_error);
			std::string message = open_error ? open_error : "Open failed without message";
			duckdb_free(open_error);
			if (status != DuckDBSuccess) {
				throw std::runtime_error(message);
			}
			checked(pdd_bridge_filesystem_refresh(handle, &error), error);
			require(duckdb_connect(handle, &connection) == DuckDBSuccess, "Cannot connect");
		} catch (...) {
			duckdb_disconnect(&connection);
			duckdb_close(&handle);
			throw;
		}
	}
	~Database() {
		duckdb_disconnect(&connection);
		duckdb_close(&handle);
	}
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
};

struct Result {
	duckdb_result value = {};
	Result(duckdb_connection connection, const std::string &sql) {
		if (duckdb_query(connection, sql.c_str(), &value) != DuckDBSuccess) {
			std::string message = duckdb_result_error(&value);
			duckdb_destroy_result(&value);
			throw std::runtime_error(message);
		}
	}
	~Result() { duckdb_destroy_result(&value); }
	Result(const Result &) = delete;
	Result &operator=(const Result &) = delete;
};

void query(Database &database, const std::string &sql) { Result result(database.connection, sql); }

int64_t scalar(Database &database, const std::string &sql) {
	Result result(database.connection, sql);
	require(duckdb_row_count(&result.value) == 1, "Expected a scalar row");
	return duckdb_value_int64(&result.value, 0, 0);
}

void refused(Database &database, const std::string &sql, const char *reason) {
	duckdb_result result = {};
	auto status = duckdb_query(database.connection, sql.c_str(), &result);
	std::string error = duckdb_result_error(&result) ? duckdb_result_error(&result) : "";
	duckdb_destroy_result(&result);
	require(status == DuckDBError, "Competing or replaced database was admitted");
	if (error.find(reason) == std::string::npos) {
		throw std::runtime_error("Unexpected refusal: " + error);
	}
}

void basic() {
	Directory directory;
	const auto original = directory.path / "original.duckdb";
	const auto alias = directory.path / "alias.duckdb";
	Cache cache;
	{
		Database first(cache, original), other(cache, directory.path / "other.duckdb");
		query(first, "CREATE TABLE t(id INTEGER); INSERT INTO t VALUES (1); CHECKPOINT");
		std::filesystem::create_hard_link(original, alias);
		Database shared(cache, alias);
		query(first, "BEGIN; INSERT INTO t VALUES (2)");
		require(scalar(shared, "SELECT count(*) FROM t") == 1, "Uncommitted write leaked across connections");
		query(first, "ROLLBACK; INSERT INTO t VALUES (2)");
		require(scalar(shared, "SELECT sum(id) FROM t") == 3,
				"Committed write is missing through hardlink cache reuse");
		query(shared, "INSERT INTO t VALUES (3)");
		refused(other, "ATTACH " + quote(alias) + " AS alias_db", "Unique file handle conflict");
		refused(other, "ATTACH " + quote("file://" + alias.u8string()) + " AS uri_alias",
				"Unique file handle conflict");
		const auto csv = directory.path / "ordinary.csv";
		query(other, "COPY (SELECT 42 AS value) TO " + quote(csv) + " (HEADER)");
		require(scalar(other, "SELECT value FROM read_csv_auto(" + quote(csv) + ")") == 42,
				"Ordinary filesystem I/O changed");
		bool incompatible = false;
		try {
			Database readonly(cache, alias, "read_only");
		} catch (const std::exception &error) {
			incompatible = std::string(error.what()).find("different configuration") != std::string::npos;
		}
		require(incompatible, "Incompatible cache configuration was accepted");
	}
	Database reopened(cache, original);
	require(scalar(reopened, "SELECT sum(id) FROM t") == 6, "Committed writes were lost after reopen");
	Database memory_a(cache, ":memory:"), memory_b(cache, ":memory:");
	query(memory_a, "CREATE TABLE only_here(id INTEGER)");
	require(scalar(memory_b, "SELECT count(*) FROM information_schema.tables WHERE table_name='only_here'") == 0,
			"Unnamed memory databases were shared");
}

void replacement(bool detach) {
	Directory directory;
	const auto original = directory.path / "original.duckdb";
	const auto alias = directory.path / "alias.duckdb";
	Cache cache;
	{
		Database primary(cache, directory.path / "primary.duckdb");
		query(primary, "ATTACH " + quote(original) +
						   " AS old; CREATE TABLE old.t(id INTEGER); INSERT INTO old.t VALUES (1); CHECKPOINT old");
		std::filesystem::create_hard_link(original, alias);
		if (detach) {
			query(primary, "DETACH old");
		}
		std::filesystem::rename(original, directory.path / "moved.duckdb");
		refused(primary, "ATTACH " + quote(alias) + " AS shadow", "replaced or unlinked");
		{
			duckdb_database replacement = nullptr;
			duckdb_connection connection = nullptr;
			require(duckdb_open(original.u8string().c_str(), &replacement) == DuckDBSuccess,
					"Cannot create replacement");
			require(duckdb_connect(replacement, &connection) == DuckDBSuccess, "Cannot connect replacement");
			{ Result result(connection, "CREATE TABLE t(id INTEGER); INSERT INTO t VALUES (99); CHECKPOINT"); }
			duckdb_disconnect(&connection);
			duckdb_close(&replacement);
		}
		refused(primary, "ATTACH " + quote(alias) + " AS shadow", "replaced or unlinked");
		refused(primary, "ATTACH " + quote(original) + " AS replacement", "replaced or unlinked");
		refused(primary, "ATTACH " + quote("file://" + alias.u8string()) + " AS uri_alias", "replaced or unlinked");
		refused(primary, "ATTACH " + quote("file://" + original.u8string()) + " AS uri_replacement",
				"replaced or unlinked");
		bool replaced_primary_refused = false;
		try {
			Database wrong(cache, original);
		} catch (const std::exception &error) {
			replaced_primary_refused = std::string(error.what()).find("replaced or unlinked") != std::string::npos;
		}
		require(replaced_primary_refused, "Replaced primary path bypassed refusal");
	}
	Database old_file(cache, alias), new_file(cache, original);
	require(scalar(old_file, "SELECT id FROM t") == 1, "Hardlink opened the replacement database after owner cleanup");
	require(scalar(new_file, "SELECT id FROM t") == 99, "Closing owners did not allow the replacement database");
}

void concurrent_aliases() {
	Directory directory;
	Cache cache;
	Database original(cache, directory.path / "original.duckdb");
	query(original, "CREATE TABLE t(id INTEGER); CHECKPOINT");
	std::vector<std::filesystem::path> aliases;
	for (int index = 0; index < 8; index++) {
		auto alias = directory.path / ("alias-" + std::to_string(index) + ".duckdb");
		aliases.push_back(alias);
	}
	std::mutex mutex;
	std::vector<std::string> errors;
	std::vector<std::thread> workers;
	for (int index = 0; index < 8; index++) {
		workers.emplace_back([&, index] {
			try {
				std::filesystem::create_hard_link(directory.path / "original.duckdb",
												  aliases[static_cast<size_t>(index)]);
				Database shared(cache, aliases[static_cast<size_t>(index)]);
				query(shared, "INSERT INTO t VALUES (" + std::to_string(index) + ")");
			} catch (const std::exception &error) {
				std::lock_guard<std::mutex> lock(mutex);
				errors.emplace_back(error.what());
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	if (!errors.empty()) {
		throw std::runtime_error("Concurrent cached alias failed: " + errors.front());
	}
	require(scalar(original, "SELECT count(*) FROM t") == 8, "Concurrent aliases lost committed rows");
	require(scalar(original, "SELECT sum(id) FROM t") == 28, "Concurrent aliases returned incorrect values");
}

void retained_attachment_owner() {
	Directory directory;
	const auto original = directory.path / "original.duckdb";
	const auto alias = directory.path / "alias.duckdb";
	Cache cache;
	{
		Database seed(cache, original);
		query(seed, "CREATE TABLE t(id INTEGER); INSERT INTO t VALUES (7); CHECKPOINT");
	}
	std::filesystem::create_hard_link(original, alias);
	auto first = std::make_unique<Database>(cache, original, "read_only");
	Database attachment(cache, directory.path / "attachment.duckdb");
	query(attachment, "ATTACH " + quote(alias) + " AS other (READ_ONLY)");
	first.reset();
	require(scalar(attachment, "SELECT id FROM other.t") == 7, "Original filesystem owner closed a live attachment");
	Database writer(cache, directory.path / "writer.duckdb");
	refused(writer, "ATTACH " + quote(alias) + " AS competing", "Unique file handle conflict");
}

#ifndef _WIN32
void failed_attach_does_not_poison_opens() {
	Directory directory;
	Cache cache;
	Database database(cache, directory.path / "primary.duckdb");
	query(database, "CREATE TABLE t(id INTEGER)");
	const auto pending = directory.path / "pending";
	std::filesystem::create_directory(pending);
	refused(database, "ATTACH " + quote(pending / "missing.duckdb") + " AS missing (READ_ONLY)", "does not exist");
	std::filesystem::rename(pending, directory.path / "pending-moved");
	std::filesystem::create_directory_symlink("pending", pending);
	query(database, "INSERT INTO t VALUES (123)");
	require(scalar(database, "SELECT count(*) FROM t") == 1, "Pending inspection changed committed INSERT behavior");
	Database unrelated(cache, directory.path / "separate-owner.duckdb");
	require(scalar(unrelated, "SELECT 42") == 42, "Failed ATTACH poisoned an unrelated owner's open");
	query(database, "ATTACH " + quote(directory.path / "unrelated.duckdb") + " AS unrelated");
	query(database, "CREATE TABLE unrelated.confirmed(id INTEGER)");
	std::filesystem::remove(pending);
	std::filesystem::rename(directory.path / "pending-moved", pending);
}
#endif

struct PausedOpen {
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool released = false;

	void Wait() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		condition.notify_all();
		condition.wait(lock, [&] { return released; });
	}
	void AwaitEntry() {
		std::unique_lock<std::mutex> lock(mutex);
		require(condition.wait_for(lock, std::chrono::seconds(30), [&] { return entered; }),
				"Attach never reached file open");
	}
	void Release() {
		std::lock_guard<std::mutex> lock(mutex);
		released = true;
		condition.notify_all();
	}
};

class PausingFileSystem final : public duckdb::LocalFileSystem {
  public:
	PausingFileSystem(std::string path_p, std::shared_ptr<PausedOpen> pause_p)
		: path(std::move(path_p)), pause(std::move(pause_p)) {}
	bool CanHandleFile(const std::string &name) override { return name == path; }
	std::string GetName() const override { return "pdo_filesystem_paused_open"; }
	duckdb::unique_ptr<duckdb::FileHandle> OpenFile(const std::string &name, duckdb::FileOpenFlags flags,
													duckdb::optional_ptr<duckdb::FileOpener> opener) override {
		if (flags.OpenForWriting()) {
			pause->Wait();
		}
		return duckdb::LocalFileSystem::OpenFile(name, flags, opener);
	}

  private:
	std::string path;
	std::shared_ptr<PausedOpen> pause;
};

void failed_sibling_preserves_pending_open() {
	Directory directory;
	Cache cache;
	Database owner(cache, ":memory:");
	const auto pending = directory.path / "pending";
	std::filesystem::create_directory(pending);
	const auto path = pending / "data.duckdb";
	const auto alias = directory.path / "alias.duckdb";
	auto pause = std::make_shared<PausedOpen>();
	auto &wrapper = *reinterpret_cast<duckdb::DatabaseWrapper *>(owner.handle);
	auto &config = duckdb::DBConfig::GetConfig(*wrapper.database->instance);
	config.file_system->RegisterSubSystem(duckdb::make_uniq<PausingFileSystem>(path.u8string(), pause));
	duckdb_connection sibling = nullptr;
	require(duckdb_connect(owner.handle, &sibling) == DuckDBSuccess, "Cannot connect sibling");
	std::string worker_error;
	bool parent_replaced = false;
	std::thread worker([&] {
		try {
			query(owner, "ATTACH " + quote(path) + " AS held");
		} catch (const std::exception &error) {
			worker_error = error.what();
		}
	});
	try {
		pause->AwaitEntry();
		duckdb_result result = {};
		auto sql = "ATTACH " + quote(path) + " AS sibling (READ_ONLY)";
		auto status = duckdb_query(sibling, sql.c_str(), &result);
		std::string sibling_error = duckdb_result_error(&result) ? duckdb_result_error(&result) : "";
		duckdb_destroy_result(&result);
		require(status == DuckDBError && sibling_error.find("Unique file handle conflict") != std::string::npos,
				"Sibling did not encounter the active native reservation");
#ifndef _WIN32
		std::filesystem::rename(pending, directory.path / "pending-moved");
		parent_replaced = true;
		std::filesystem::create_directory_symlink("pending", pending);
		{
			Result create(sibling, "CREATE TABLE committed(id INTEGER)");
			Result insert(sibling, "INSERT INTO committed VALUES (123)");
			Result count(sibling, "SELECT count(*) FROM committed");
			require(duckdb_value_int64(&count.value, 0, 0) == 1, "Pending inspection falsely failed a committed write");
		}
		bool uncertainty_refused = false;
		try {
			Database unrelated(cache, directory.path / "uncertain.duckdb");
		} catch (const std::exception &error) {
			uncertainty_refused =
				std::string(error.what()).find("Cannot inspect database filesystem identity") != std::string::npos;
		}
		require(uncertainty_refused, "Failed sibling retired the uncertain live pending reference");
		require(!std::filesystem::exists(directory.path / "uncertain.duckdb"),
				"Uncertain pending open created a database");
		std::filesystem::remove(pending);
		std::filesystem::rename(directory.path / "pending-moved", pending);
		parent_replaced = false;
#endif
		pause->Release();
		worker.join();
		duckdb_disconnect(&sibling);
		if (!worker_error.empty()) {
			throw std::runtime_error("Paused ATTACH failed: " + worker_error);
		}
	} catch (...) {
		if (parent_replaced) {
			std::error_code ignored;
			std::filesystem::remove(pending, ignored);
			std::filesystem::rename(directory.path / "pending-moved", pending, ignored);
		}
		pause->Release();
		if (worker.joinable()) {
			worker.join();
		}
		duckdb_disconnect(&sibling);
		throw;
	}
	query(owner, "CREATE TABLE held.t(id INTEGER); INSERT INTO held.t VALUES (17); CHECKPOINT held");
	std::filesystem::create_hard_link(path, alias);
	Database other(cache, ":memory:");
	refused(other, "ATTACH " + quote(alias) + " AS competing", "Unique file handle conflict");
	require(scalar(owner, "SELECT id FROM held.t") == 17, "Failed sibling damaged the successful attachment");
}

} // namespace

int main() {
	try {
		pdd_bridge_error error = {};
		checked(pdd_bridge_check(&error), error);
		basic();
		concurrent_aliases();
		retained_attachment_owner();
		failed_sibling_preserves_pending_open();
#ifndef _WIN32
		replacement(false);
		replacement(true);
		failed_attach_does_not_poison_opens();
#else
		std::puts("filesystem: live rename/replacement cases not executed on Windows");
#endif
		std::puts("filesystem identity: cache, ATTACH, transactions, durability, ordinary I/O, concurrent aliases, "
				  "cleanup passed");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "filesystem identity: %s\n", error.what());
		return 1;
	}
}
