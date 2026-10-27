#ifndef PDO_DUCKDB_BRIDGE_INTERNAL_HPP
#define PDO_DUCKDB_BRIDGE_INTERNAL_HPP

#include "pdo_duckdb_bridge.h"
#include "pdo_duckdb_bridge_build.h"
#include "duckdb.hpp"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pdd_bridge {
void check_identity();
duckdb::Value geometry_as_hex(const duckdb::Value &value);
duckdb::Value variant_geometry_as_hex(const duckdb::Value &value);
duckdb::string quote_nested(const duckdb::string &value, bool key);
duckdb::string variant_render(const duckdb::Value &value, duckdb_connection connection, bool nested = false);

inline void require(bool condition, const char *message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

template <class OUTPUT>
inline void bytes(OUTPUT *out, const std::string &value) {
    require(out != nullptr, "Missing output pointer");
    if (value.size() == std::numeric_limits<size_t>::max()) {
        throw std::length_error("String exceeds bridge allocation limit");
    }
    auto copy = static_cast<char *>(std::malloc(value.size() + 1));
    if (!copy) {
        throw std::bad_alloc();
    }
    std::memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    out->data = copy;
    out->length = value.size();
}

inline void set_error(pdd_bridge_error *error, const char *message) noexcept {
    if (!error) {
        return;
    }
    const auto length = std::strlen(message);
    auto copy = static_cast<char *>(std::malloc(length + 1));
    if (copy) {
        std::memcpy(copy, message, length + 1);
        error->data = copy;
        error->length = length;
    }
}

template <class F>
inline pdd_bridge_status guard(pdd_bridge_error *error, F body) noexcept {
    if (error) {
        error->data = nullptr;
        error->length = 0;
    }
    try {
        check_identity();
        body();
        return PDD_BRIDGE_OK;
    } catch (const std::exception &exception) {
        set_error(error, exception.what());
    } catch (...) {
        set_error(error, "Unknown exception in DuckDB bridge");
    }
    return PDD_BRIDGE_ERROR;
}

}
#endif
