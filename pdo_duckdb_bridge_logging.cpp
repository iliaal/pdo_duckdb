#include "pdo_duckdb_bridge_logging.h"
#include "pdo_duckdb_bridge_internal.hpp"

#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/string_util.hpp"

// DuckDB 2.0.1 renamed LogStorage to LogSink (duckdb/duckdb#26085) without changing its layout or behavior.
#if __has_include("duckdb/logging/log_sink.hpp")
#define PDD_BRIDGE_HAVE_LOG_SINK 1
#endif

#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace {

#ifdef PDD_BRIDGE_HAVE_LOG_SINK
using LogSink = duckdb::LogSink;
using BufferingLogSink = duckdb::BufferingLogSink;
using FileLogSink = duckdb::FileLogSink;
using InMemoryLogSink = duckdb::InMemoryLogSink;
#else
using LogSink = duckdb::LogStorage;
using BufferingLogSink = duckdb::BufferingLogStorage;
using FileLogSink = duckdb::FileLogStorage;
using InMemoryLogSink = duckdb::InMemoryLogStorage;
#endif

class DiscardLogStorage final : public LogSink {
public:
#ifdef PDD_BRIDGE_HAVE_LOG_SINK
	const duckdb::string GetSinkName() override {
#else
	const duckdb::string GetStorageName() override {
#endif
		return "DiscardLogStorage";
	}
	void WriteLogEntry(duckdb::timestamp_t, duckdb::LogLevel, const duckdb::string &, const duckdb::string &,
		const duckdb::RegisteredLoggingContext &) noexcept override {
	}
	void WriteLogEntries(duckdb::DataChunk &, const duckdb::RegisteredLoggingContext &) noexcept override {
	}
	void FlushAll() noexcept override {
	}
	void Flush(duckdb::LoggingTargetTable) noexcept override {
	}
	void Truncate() noexcept override {
	}
	bool IsEnabled(duckdb::LoggingTargetTable) noexcept override {
		return false;
	}
};

class LogManagerMembers : public duckdb::LogManager {
public:
	static auto Mutex() noexcept {
		return &LogManagerMembers::lock;
	}
	static auto Storage() noexcept {
#ifdef PDD_BRIDGE_HAVE_LOG_SINK
		return &LogManagerMembers::log_sink;
#else
		return &LogManagerMembers::log_storage;
#endif
	}
	static auto Config() noexcept {
		return &LogManagerMembers::config;
	}
};

class BufferingStorageMembers : public BufferingLogSink {
public:
	static auto Mutex() noexcept {
		return &BufferingStorageMembers::lock;
	}
};

struct FileStorageTables {
	friend auto tables_member(FileStorageTables) noexcept;
};

template <auto Member>
struct FileStorageTablesAccess {
	friend auto tables_member(FileStorageTables) noexcept {
		return Member;
	}
};

// Explicit instantiation permits this private member pointer ([temp.spec.general]); no object layout is copied.
template struct FileStorageTablesAccess<&FileLogSink::tables>;

static_assert(std::is_same<decltype(LogManagerMembers::Mutex()), duckdb::mutex duckdb::LogManager::*>::value,
	"Log manager access must use a base member pointer, not a downcast");
static_assert(std::is_same<decltype(LogManagerMembers::Storage()),
	duckdb::shared_ptr<LogSink> duckdb::LogManager::*>::value, "Log storage member type changed");
static_assert(std::is_same<decltype(BufferingStorageMembers::Mutex()),
	duckdb::mutex BufferingLogSink::*>::value, "Storage mutex must use a base member pointer");

duckdb::shared_ptr<LogSink> discard_storage() noexcept {
	// An allocation-failure fallback must outlive every engine handle without heap allocation or destructor ordering.
	alignas(DiscardLogStorage) static unsigned char memory[sizeof(DiscardLogStorage)];
	static auto *storage = new (memory) DiscardLogStorage();
	return duckdb::shared_ptr<LogSink>(duckdb::shared_ptr<LogSink>(), storage);
}

void revoke_file_storage(FileLogSink &file_storage) {
	std::lock_guard<duckdb::mutex> lock(file_storage.*BufferingStorageMembers::Mutex());
	for (auto &entry : file_storage.*tables_member(FileStorageTables())) {
		// Retained binders must fail in Initialize before touching CSVWriter's borrowed WriteStream.
		entry.second.path.clear();
		entry.second.initialized = false;
		entry.second.file_writer.reset();
	}
}

bool detach_storage(duckdb::LogManager &manager) {
	duckdb::shared_ptr<LogSink> previous;
	bool mismatched = false;
	{
		std::lock_guard<duckdb::mutex> lock(manager.*LogManagerMembers::Mutex());
		const auto &storage_name = (manager.*LogManagerMembers::Config()).storage;
		auto *storage = (manager.*LogManagerMembers::Storage()).get();
		auto *file_storage = dynamic_cast<FileLogSink *>(storage);
		const bool named_memory = duckdb::StringUtil::CIEquals(storage_name.data(), storage_name.size(), "memory", 6);
		const bool named_file = duckdb::StringUtil::CIEquals(storage_name.data(), storage_name.size(), "file", 4);
		if (named_memory && dynamic_cast<InMemoryLogSink *>(storage)) {
			return false;
		}
		mismatched = named_memory || (named_file != (file_storage != nullptr));
		if (file_storage) {
			revoke_file_storage(*file_storage);
		}
		previous = std::move(manager.*LogManagerMembers::Storage());
		manager.*LogManagerMembers::Storage() = discard_storage();
		(manager.*LogManagerMembers::Config()).storage.clear();
	}
	// Public storage switching flushes the previous sink; destruction instead discards its pending records.
	previous.reset();
	pdd_bridge::require(!mismatched, "Inconsistent DuckDB log storage configuration");
	return true;
}

} // namespace

extern "C" pdd_bridge_status pdd_bridge_clear_instance_writers(duckdb_connection handle, pdd_bridge_error *error) {
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(handle != nullptr, "Connection must not be NULL");
		auto &context = *reinterpret_cast<duckdb::Connection *>(handle)->context;
		auto &manager = duckdb::LogManager::Get(context);
		if (!detach_storage(manager)) {
			return;
		}
		manager.SetEnableLogging(false);
#ifdef PDD_BRIDGE_HAVE_LOG_SINK
		manager.SetLogSink(*context.db, "memory");
#else
		manager.SetLogStorage(*context.db, "memory");
#endif
	});
}
