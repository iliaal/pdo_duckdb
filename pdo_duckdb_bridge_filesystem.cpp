#include "pdo_duckdb_bridge_filesystem.h"
#include "pdo_duckdb_bridge_internal.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/extension_callback.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

enum class FileKind { MISSING, REGULAR, OTHER };

struct Identity {
	FileKind kind = FileKind::MISSING;
	uint64_t device = 0;
	uint64_t file = 0;

	bool Matches(const Identity &other) const {
		return kind == FileKind::REGULAR && other.kind == FileKind::REGULAR && device == other.device &&
			   file == other.file;
	}
};

Identity identify(const std::string &path) {
#ifdef _WIN32
	pdd_bridge::require(path.size() <= static_cast<size_t>(INT_MAX),
						"Filesystem path exceeds Windows conversion limit");
	int length =
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
	if (!length) {
		throw std::runtime_error("Cannot decode database filesystem path");
	}
	std::wstring wide(static_cast<size_t>(length), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0],
							 length)) {
		throw std::runtime_error("Cannot decode database filesystem path");
	}
	HANDLE handle = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
								OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			return {};
		}
		throw std::runtime_error("Cannot inspect database filesystem identity (Windows error " + std::to_string(error) +
								 ")");
	}
	BY_HANDLE_FILE_INFORMATION info;
	BOOL success = GetFileInformationByHandle(handle, &info);
	DWORD error = success ? ERROR_SUCCESS : GetLastError();
	CloseHandle(handle);
	if (!success) {
		throw std::runtime_error("Cannot inspect database filesystem identity (Windows error " + std::to_string(error) +
								 ")");
	}
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		return {FileKind::OTHER, 0, 0};
	}
	return {FileKind::REGULAR, info.dwVolumeSerialNumber,
			(static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
	struct stat info;
	if (stat(path.c_str(), &info) != 0) {
		if (errno == ENOENT || errno == ENOTDIR) {
			return {};
		}
		throw std::runtime_error("Cannot inspect database filesystem identity: " + std::string(std::strerror(errno)));
	}
	if (!S_ISREG(info.st_mode)) {
		return {FileKind::OTHER, 0, 0};
	}
	return {FileKind::REGULAR, static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino)};
#endif
}

class Owner;
struct QueryToken {};

struct PendingReference {
	const Owner *owner;
	std::shared_ptr<const QueryToken> query;
};

struct Entry {
	std::string path;
	Identity identity;
	std::vector<const Owner *> owners;
	std::vector<PendingReference> pending;

	bool HasOwner(const Owner *owner) const { return std::find(owners.begin(), owners.end(), owner) != owners.end(); }
	void AddOwner(const Owner *owner) {
		if (!HasOwner(owner)) {
			owners.push_back(owner);
		}
	}
	bool HasPending(const Owner *owner, const std::shared_ptr<const QueryToken> &query) const {
		return std::any_of(pending.begin(), pending.end(), [&](const PendingReference &reference) {
			return reference.owner == owner && reference.query == query;
		});
	}
	void AddReference(const Owner *owner, const std::shared_ptr<const QueryToken> &query) {
		if (identity.kind == FileKind::REGULAR) {
			AddOwner(owner);
		} else if (!HasPending(owner, query)) {
			pending.push_back({owner, query});
		}
	}
	bool IsUnused() const { return owners.empty() && pending.empty(); }
	void Capture(const Identity &observed) {
		for (const auto &reference : pending) {
			AddOwner(reference.owner);
		}
		identity = observed;
		pending.clear();
	}
};

class Registry {
  public:
	void Validate(const std::string &path) {
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &entry : entries) {
			if (entry.path == path) {
				ValidateEntry(entry);
			}
		}
	}

	std::string Canonicalize(const Owner *owner, const std::string &path,
							 const std::shared_ptr<const QueryToken> &query) {
		std::lock_guard<std::mutex> lock(mutex);
		CapturePending(nullptr);
		auto identity = identify(path);
		for (auto &entry : entries) {
			if (entry.path == path || entry.identity.Matches(identity)) {
				ValidateEntry(entry);
				entry.AddReference(owner, query);
				return entry.path;
			}
		}
		if (identity.kind == FileKind::OTHER) {
			return path;
		}
		Entry entry = {path, identity, {}, {}};
		entry.AddReference(owner, query);
		entries.push_back(std::move(entry));
		return path;
	}

	void Refresh(const Owner *owner) {
		std::lock_guard<std::mutex> lock(mutex);
		CapturePending(owner);
	}

	void Remove(const Owner *owner) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &entry : entries) {
			entry.owners.erase(std::remove(entry.owners.begin(), entry.owners.end(), owner), entry.owners.end());
			entry.pending.erase(
				std::remove_if(entry.pending.begin(), entry.pending.end(),
							   [&](const PendingReference &reference) { return reference.owner == owner; }),
				entry.pending.end());
		}
		RemoveUnused();
	}

	void Complete(const Owner *owner, const std::shared_ptr<const QueryToken> &query, bool failed) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &entry : entries) {
			if (!entry.HasPending(owner, query)) {
				continue;
			}
			try {
				auto identity = identify(entry.path);
				if (identity.kind == FileKind::REGULAR) {
					entry.Capture(identity);
				} else if (identity.kind == FileKind::MISSING && failed) {
					entry.pending.erase(std::remove_if(entry.pending.begin(), entry.pending.end(),
													   [&](const PendingReference &reference) {
														   return reference.owner == owner && reference.query == query;
													   }),
										entry.pending.end());
				}
			} catch (...) {
				// Sampling uncertainty keeps the reference pending, independently of the SQL outcome.
			}
		}
		RemoveUnused();
	}

  private:
	static void ValidateEntry(const Entry &entry) {
		if (entry.identity.kind == FileKind::REGULAR && !entry.identity.Matches(identify(entry.path))) {
			throw std::runtime_error(
				"Database path was replaced or unlinked; close its filesystem owners before reopening");
		}
	}

	void CapturePending(const Owner *owner) {
		for (auto &entry : entries) {
			if (entry.identity.kind == FileKind::MISSING &&
				(!owner || std::any_of(entry.pending.begin(), entry.pending.end(),
									   [&](const PendingReference &reference) { return reference.owner == owner; }))) {
				auto identity = identify(entry.path);
				if (identity.kind == FileKind::REGULAR) {
					entry.Capture(identity);
				}
			}
		}
	}
	void RemoveUnused() {
		entries.erase(
			std::remove_if(entries.begin(), entries.end(), [](const Entry &entry) { return entry.IsUnused(); }),
			entries.end());
	}

	std::mutex mutex;
	std::vector<Entry> entries;
};

std::shared_ptr<Registry> shared_registry() {
	static std::mutex mutex;
	static std::weak_ptr<Registry> weak;
	std::lock_guard<std::mutex> lock(mutex);
	auto registry = weak.lock();
	if (!registry) {
		registry = std::make_shared<Registry>();
		weak = registry;
	}
	return registry;
}

class Owner {
  public:
	Owner() : registry(shared_registry()) {}
	~Owner() { registry->Remove(this); }
	Owner(const Owner &) = delete;
	Owner &operator=(const Owner &) = delete;
	std::string Canonicalize(const std::string &path, const std::shared_ptr<const QueryToken> &query) {
		return registry->Canonicalize(this, path, query);
	}
	void Validate(const std::string &path) { registry->Validate(path); }
	void Refresh() { registry->Refresh(this); }
	void Complete(const std::shared_ptr<const QueryToken> &query, bool failed) {
		registry->Complete(this, query, failed);
	}

  private:
	std::shared_ptr<Registry> registry;
};

class CaptureCreatedFiles final : public duckdb::ClientContextState {
  public:
	explicit CaptureCreatedFiles(std::weak_ptr<Owner> owner_p) : owner(std::move(owner_p)) {}
	void QueryBegin(duckdb::ClientContext &) override {
		std::lock_guard<std::mutex> lock(mutex);
		active = true;
		query.reset();
	}
	std::shared_ptr<const QueryToken> ActiveQuery() {
		std::lock_guard<std::mutex> lock(mutex);
		if (active && !query) {
			query = std::make_shared<QueryToken>();
		}
		return query;
	}
	void QueryEnd(duckdb::ClientContext &, duckdb::optional_ptr<duckdb::ErrorData> error) noexcept override {
		try {
			std::shared_ptr<const QueryToken> completed;
			{
				std::lock_guard<std::mutex> lock(mutex);
				active = false;
				completed.swap(query);
			}
			if (completed) {
				if (auto current = owner.lock()) {
					current->Complete(completed, error && error->HasError());
				}
			}
		} catch (...) {
			// QueryEnd follows commit. Keep the sample pending; the next canonicalization retries before opening.
		}
	}

  private:
	std::weak_ptr<Owner> owner;
	std::mutex mutex;
	std::shared_ptr<const QueryToken> query;
	bool active = false;
};

class IdentityFileSystem final : public duckdb::VirtualFileSystem {
  public:
	explicit IdentityFileSystem(std::shared_ptr<Owner> owner_p) : owner(std::move(owner_p)) {}
	std::string CanonicalizePath(const std::string &path, duckdb::optional_ptr<duckdb::FileOpener> opener) override {
		auto canonical = duckdb::VirtualFileSystem::CanonicalizePath(path, opener);
		auto expanded = duckdb::FileSystem::ExpandPath(path, opener);
		auto parsed = duckdb::Path::FromString(expanded);
		if (!parsed.IsLocal() || parsed.HasScheme()) {
			return canonical;
		}
		duckdb::LocalFileSystem local;
		auto absolute = local.MakePathAbsolute(expanded, opener);
		owner->Validate(duckdb::FileSystem::CanonicalizePath(absolute, opener));
		std::shared_ptr<const QueryToken> query;
		if (auto context = duckdb::FileOpener::TryGetClientContext(opener)) {
			if (auto state = context->registered_state->Get<CaptureCreatedFiles>("pdo_duckdb_filesystem_identity")) {
				query = state->ActiveQuery();
			}
		}
		return owner->Canonicalize(canonical, query);
	}
	void Refresh() { owner->Refresh(); }

  private:
	std::shared_ptr<Owner> owner;
};

class RegisterCapture final : public duckdb::ExtensionCallback {
  public:
	explicit RegisterCapture(std::weak_ptr<Owner> owner_p) : owner(std::move(owner_p)) {}
	void OnConnectionOpened(duckdb::ClientContext &context) override {
		context.registered_state->Insert("pdo_duckdb_filesystem_identity",
										 duckdb::make_shared_ptr<CaptureCreatedFiles>(owner));
	}

  private:
	std::weak_ptr<Owner> owner;
};

} // namespace

extern "C" pdd_bridge_status pdd_bridge_filesystem_install(duckdb_config handle, pdd_bridge_error *error) {
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(handle != nullptr, "Config must not be NULL");
		// The matching v1 C API represents duckdb_config as DBConfig *.
		auto &config = *reinterpret_cast<duckdb::DBConfig *>(handle);
		if (dynamic_cast<IdentityFileSystem *>(config.file_system.get())) {
			return;
		}
		pdd_bridge::require(!config.file_system, "Cannot replace a configured DuckDB filesystem");
		auto owner = std::make_shared<Owner>();
		auto filesystem = duckdb::make_uniq<IdentityFileSystem>(owner);
		duckdb::ExtensionCallback::Register(config, duckdb::make_shared_ptr<RegisterCapture>(owner));
		config.file_system = std::move(filesystem);
	});
}

extern "C" pdd_bridge_status pdd_bridge_filesystem_refresh(duckdb_database handle, pdd_bridge_error *error) {
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(handle != nullptr, "Database must not be NULL");
		// DatabaseWrapper is the matching v1 C API's actual owning database representation.
		auto &wrapper = *reinterpret_cast<duckdb::DatabaseWrapper *>(handle);
		pdd_bridge::require(wrapper.database && wrapper.database->instance, "Database wrapper is empty");
		auto &config = duckdb::DBConfig::GetConfig(*wrapper.database->instance);
		auto filesystem = dynamic_cast<IdentityFileSystem *>(config.file_system.get());
		pdd_bridge::require(filesystem != nullptr, "Database has no PDO filesystem identity guard");
		filesystem->Refresh();
	});
}
