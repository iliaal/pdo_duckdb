#include "pdo_duckdb_bridge_internal.hpp"

namespace pdd_bridge {
void check_identity() {
    /* The v1 C handle mappings are defined by the matching src/main/capi implementation. */
    if (std::strcmp(duckdb::DuckDB::LibraryVersion(), PDO_DUCKDB_BRIDGE_EXPECTED_VERSION) != 0 ||
        std::strcmp(duckdb::DuckDB::SourceID(), PDO_DUCKDB_BRIDGE_EXPECTED_SOURCE_ID) != 0) {
        throw std::runtime_error("DuckDB bridge header/library identity mismatch: expected "
                                 PDO_DUCKDB_BRIDGE_EXPECTED_VERSION " ("
                                 PDO_DUCKDB_BRIDGE_EXPECTED_SOURCE_ID ")");
    }
}
}

extern "C" {
pdd_bridge_status pdd_bridge_check(pdd_bridge_error *error) {
    return pdd_bridge::guard(error, [] {});
}

void pdd_bridge_bytes_free(pdd_bridge_bytes *bytes) {
    if (bytes) {
        std::free(bytes->data);
        bytes->data = nullptr;
        bytes->length = 0;
    }
}

void pdd_bridge_error_free(pdd_bridge_error *error) {
    if (error) {
        std::free(error->data);
        error->data = nullptr;
        error->length = 0;
    }
}
}
