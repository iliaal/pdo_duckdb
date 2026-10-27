#include "pdo_duckdb_bridge_types.h"
#include "pdo_duckdb_bridge_internal.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include <memory>
#include <vector>
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR < 2
#include "duckdb/common/extra_type_info.hpp"
#endif

struct pdd_bridge_value {
    duckdb::Value value;
    explicit pdd_bridge_value(duckdb::Value value_p) : value(std::move(value_p)) {}
};

namespace pdd_bridge {
static void validate_type(const duckdb::LogicalType &root) {
    duckdb::vector<const duckdb::LogicalType *> pending {&root};
    while (!pending.empty()) {
        auto &type = *pending.back();
        pending.pop_back();
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        using TypeInfo = duckdb::LogicalTypeInfoType;
        auto info = type.GetTypeInfo().type;
#else
        using TypeInfo = duckdb::ExtraTypeInfoType;
        auto info = type.AuxInfo() ? type.AuxInfo()->type : TypeInfo::INVALID_TYPE_INFO;
#endif
        switch (type.InternalType()) {
            case duckdb::PhysicalType::STRUCT:
                require(info == TypeInfo::STRUCT_TYPE_INFO, "Incomplete nested logical type");
                for (auto &field : duckdb::StructType::GetChildTypes(type)) {
                    pending.push_back(&field.second);
                }
                break;
            case duckdb::PhysicalType::LIST:
                require(info == TypeInfo::LIST_TYPE_INFO, "Incomplete LIST or MAP logical type");
                pending.push_back(&duckdb::ListType::GetChildType(type));
                break;
            case duckdb::PhysicalType::ARRAY:
                require(info == TypeInfo::ARRAY_TYPE_INFO, "Incomplete ARRAY logical type");
                pending.push_back(&duckdb::ArrayType::GetChildType(type));
                break;
            default: break;
        }
    }
}

static duckdb::LogicalType &logical_type(duckdb_logical_type type) {
    require(type != nullptr, "Missing logical type");
    auto &native = *reinterpret_cast<duckdb::LogicalType *>(type);
    /* duckdb_create_logical_type(VARIANT) omits required internal field types in both API generations. */
    validate_type(native);
    return native;
}

static const duckdb::Value &value(const pdd_bridge_value *input) {
    require(input != nullptr, "Missing value");
    return input->value;
}

static bool tuple(const duckdb::LogicalType &type) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    if (type.id() == duckdb::LogicalTypeId::TUPLE) {
        return true;
    }
#endif
    return type.id() == duckdb::LogicalTypeId::STRUCT && duckdb::StructType::IsUnnamed(type);
}

static bool structure(const duckdb::LogicalType &type) {
    return type.id() == duckdb::LogicalTypeId::STRUCT || tuple(type);
}

static const duckdb::vector<duckdb::Value> *children(const duckdb::Value &input) {
    if (input.IsNull()) {
        return nullptr;
    }
    if (structure(input.type())) {
        return &duckdb::StructValue::GetChildren(input);
    }
    switch (input.type().id()) {
        case duckdb::LogicalTypeId::LIST: return &duckdb::ListValue::GetChildren(input);
        case duckdb::LogicalTypeId::ARRAY: return &duckdb::ArrayValue::GetChildren(input);
        case duckdb::LogicalTypeId::MAP: return &duckdb::MapValue::GetChildren(input);
        default: return nullptr;
    }
}

static duckdb::Value cast(const duckdb::Value &input, const duckdb::LogicalType &type, duckdb_connection connection) {
    if (connection) {
        return input.CastAs(*reinterpret_cast<duckdb::Connection *>(connection)->context, type);
    }
    return input.DefaultCastAs(type);
}

static duckdb::LogicalType geometry_type(const duckdb::LogicalType &type) {
    using Id = duckdb::LogicalTypeId;
    if (type.id() == Id::GEOMETRY) {
        return duckdb::LogicalType::VARCHAR;
    }
    if (type.id() == Id::VARIANT) {
        return type;
    }
    if (type.InternalType() == duckdb::PhysicalType::STRUCT) {
        auto fields = duckdb::StructType::GetChildTypes(type);
        bool changed = false;
        for (auto &field : fields) {
            auto replacement = geometry_type(field.second);
            changed |= replacement != field.second;
            field.second = std::move(replacement);
        }
        if (!changed) {
            return type;
        }
        if (type.id() == Id::UNION) {
            fields.erase(fields.begin());
            return duckdb::LogicalType::UNION(std::move(fields));
        }
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        if (type.id() == Id::TUPLE) {
            duckdb::vector<duckdb::LogicalType> types;
            for (auto &field : fields) {
                types.push_back(std::move(field.second));
            }
            return duckdb::LogicalType::TUPLE(std::move(types));
        }
#endif
        return duckdb::LogicalType::STRUCT(std::move(fields));
    }
    if (type.id() == Id::LIST || type.id() == Id::MAP) {
        auto child = geometry_type(duckdb::ListType::GetChildType(type));
        if (child == duckdb::ListType::GetChildType(type)) {
            return type;
        }
        if (type.id() == Id::MAP) {
            return duckdb::LogicalType::MAP(duckdb::StructType::GetChildType(child, 0),
                                            duckdb::StructType::GetChildType(child, 1));
        }
        return duckdb::LogicalType::LIST(std::move(child));
    }
    if (type.id() == Id::ARRAY) {
        auto child = geometry_type(duckdb::ArrayType::GetChildType(type));
        return child == duckdb::ArrayType::GetChildType(type) ? type :
               duckdb::LogicalType::ARRAY(std::move(child), duckdb::ArrayType::GetSize(type));
    }
    return type;
}

duckdb::Value geometry_as_hex(const duckdb::Value &input) {
    using Id = duckdb::LogicalTypeId;
    auto type = geometry_type(input.type());
    if (input.IsNull()) {
        return duckdb::Value(std::move(type));
    }
    if (input.type().id() == Id::GEOMETRY) {
        auto &raw = duckdb::StringValue::Get(input);
        require(raw.size() <= std::numeric_limits<size_t>::max() / 2, "Geometry exceeds string size limit");
        duckdb::string hex(raw.size() * 2, '\0');
        static constexpr char digits[] = "0123456789ABCDEF";
        for (size_t i = 0; i < raw.size(); i++) {
            auto byte = static_cast<unsigned char>(raw[i]);
            hex[i * 2] = digits[byte >> 4];
            hex[i * 2 + 1] = digits[byte & 15];
        }
        return duckdb::Value(std::move(hex));
    }
    if (input.type().id() == Id::VARIANT) {
        return variant_geometry_as_hex(input);
    }
    if (input.type().id() == Id::UNION) {
        auto fields = duckdb::StructType::GetChildTypes(type);
        fields.erase(fields.begin());
        return duckdb::Value::UNION(std::move(fields), duckdb::UnionValue::GetTag(input),
                                   geometry_as_hex(duckdb::UnionValue::GetValue(input)));
    }
    auto input_children = children(input);
    if (!input_children) {
        return input;
    }
    duckdb::vector<duckdb::Value> values;
    values.reserve(input_children->size());
    for (auto &child : *input_children) {
        values.push_back(geometry_as_hex(child));
    }
    if (structure(type)) {
        return duckdb::Value::STRUCT(type, std::move(values));
    }
    if (type.id() == Id::ARRAY) {
        return duckdb::Value::ARRAY(duckdb::ArrayType::GetChildType(type), std::move(values));
    }
    auto result = duckdb::Value::LIST(duckdb::ListType::GetChildType(type), std::move(values));
    /* MAP and LIST share entry storage. Rendering must not reject keys that become equal only after hex conversion. */
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    return result.WithType(std::move(type));
#else
    result.Reinterpret(std::move(type));
    return result;
#endif
}

duckdb::string quote_nested(const duckdb::string &value, bool key) {
    require(value.size() <= std::numeric_limits<uint32_t>::max(), "Nested text exceeds DuckDB string length");
    duckdb::string_t text(value);
    bool quoted;
    auto length = key ? duckdb::VectorCastHelpers::CalculateEscapedStringLength<true>(text, quoted) :
                        duckdb::VectorCastHelpers::CalculateEscapedStringLength<false>(text, quoted);
    duckdb::string output(length, '\0');
    if (key) {
        duckdb::VectorCastHelpers::WriteEscapedString<true>(&output[0], text, quoted);
    } else {
        duckdb::VectorCastHelpers::WriteEscapedString<false>(&output[0], text, quoted);
    }
    return output;
}

static duckdb::string canonical_pdo(const duckdb::Value &input, duckdb_connection connection);

static bool contains_variant(const duckdb::Value &input) {
    duckdb::vector<const duckdb::Value *> pending {&input};
    while (!pending.empty()) {
        auto &value = *pending.back();
        pending.pop_back();
        if (value.IsNull()) {
            continue;
        }
        if (value.type().id() == duckdb::LogicalTypeId::VARIANT) {
            return true;
        } else if (value.type().id() == duckdb::LogicalTypeId::UNION) {
            pending.push_back(&duckdb::UnionValue::GetValue(value));
        } else if (auto items = children(value)) {
            for (auto &child : *items) {
                pending.push_back(&child);
            }
        }
    }
    return false;
}

static duckdb::string nested_pdo(const duckdb::Value &input, duckdb_connection connection, bool escape) {
    if (input.IsNull()) {
        return "NULL";
    }
    if (input.type().id() == duckdb::LogicalTypeId::VARIANT) {
        return variant_render(input, connection, escape);
    }
    auto rendered = canonical_pdo(input, connection);
    return escape && !input.type().IsNested() ? quote_nested(rendered, false) : rendered;
}

static duckdb::string canonical_pdo(const duckdb::Value &input, duckdb_connection connection) {
    if (!contains_variant(input)) {
        return duckdb::StringValue::Get(cast(input, duckdb::LogicalType::VARCHAR, connection));
    }
    using Id = duckdb::LogicalTypeId;
    if (input.type().id() == Id::VARIANT) {
        return variant_render(input, connection);
    }
    if (input.type().id() == Id::UNION) {
        return nested_pdo(duckdb::UnionValue::GetValue(input), connection, false);
    }
    auto items = children(input);
    require(items != nullptr, "VARIANT rendering requires a nested value");
    bool is_struct = structure(input.type());
    bool is_tuple = tuple(input.type());
    bool is_map = input.type().id() == Id::MAP;
    bool escape = PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2 || input.type().id() != Id::ARRAY;
    duckdb::string output = is_tuple ? "(" : (is_struct || is_map) ? "{" : "[";
    for (idx_t i = 0; i < items->size(); i++) {
        if (i) { output += ", "; }
        if (is_struct && !is_tuple) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
            output += quote_nested(duckdb::StructType::GetChildName(input.type(), i).GetIdentifierName(), true) + ": ";
#else
            output += quote_nested(duckdb::StructType::GetChildName(input.type(), i), true) + ": ";
#endif
        }
        if (is_map) {
            auto &entry = duckdb::StructValue::GetChildren((*items)[i]);
            require(entry.size() == 2, "Invalid MAP entry");
            output += nested_pdo(entry[0], connection, true) + "=" + nested_pdo(entry[1], connection, true);
        } else {
            output += nested_pdo((*items)[i], connection, escape);
        }
    }
    output += is_tuple ? ")" : (is_struct || is_map) ? "}" : "]";
    return output;
}

}

extern "C" {
pdd_bridge_status pdd_bridge_c_result_column_name(duckdb_result *result, idx_t column, pdd_bridge_bytes *out,
                                                 pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing column name output");
        pdd_bridge::require(result && result->internal_data, "Missing native result");
        /* DuckDBTranslateResult stores this exact source-matched v1 wrapper in internal_data. */
        auto &native = *static_cast<duckdb::DuckDBResultData *>(result->internal_data);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        pdd_bridge::require(native.result || native.stream, "Missing native result handle");
        pdd_bridge::require(!native.HasError(), "Cannot read column name from failed result");
        pdd_bridge::require(column < native.ColumnCount(), "Column index out of bounds");
        pdd_bridge::bytes(out, native.ColumnName(column).GetIdentifierName());
#else
        pdd_bridge::require(native.result != nullptr, "Missing native result handle");
        pdd_bridge::require(!native.result->HasError(), "Cannot read column name from failed result");
        pdd_bridge::require(column < native.result->ColumnCount(), "Column index out of bounds");
        pdd_bridge::bytes(out, native.result->names[column]);
#endif
    });
}

pdd_bridge_status pdd_bridge_type_declaration(duckdb_logical_type type, pdd_bridge_bytes *out, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] { pdd_bridge::bytes(out, pdd_bridge::logical_type(type).ToString()); });
}

pdd_bridge_status pdd_bridge_type_alias(duckdb_logical_type type, pdd_bridge_bytes *out, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] { pdd_bridge::bytes(out, pdd_bridge::logical_type(type).GetAlias()); });
}

pdd_bridge_status pdd_bridge_type_annotations(duckdb_logical_type type, pdd_bridge_annotations *out,
                                              pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing type annotations output");
        struct Frame {
            const duckdb::LogicalType *type;
            idx_t next_child = 0;
            bool visited = false;
        };
        struct Annotation {
            std::vector<idx_t> path;
            std::string collation;
        };
        std::vector<Frame> pending {{&pdd_bridge::logical_type(type)}};
        std::vector<idx_t> path;
        std::vector<Annotation> annotations;
        while (!pending.empty()) {
            auto &frame = pending.back();
            const auto &current = *frame.type;
            if (!frame.visited) {
                if (current.id() == duckdb::LogicalTypeId::VARCHAR) {
                    auto collation = duckdb::StringType::GetCollation(current);
                    if (!collation.empty()) {
                        annotations.push_back({path, std::move(collation)});
                    }
                }
                frame.visited = true;
            }
            const duckdb::LogicalType *child = nullptr;
            const auto ordinal = frame.next_child;
            if (pdd_bridge::structure(current)) {
                if (ordinal < duckdb::StructType::GetChildCount(current)) {
                    child = &duckdb::StructType::GetChildType(current, ordinal);
                }
            } else {
                switch (current.id()) {
                    case duckdb::LogicalTypeId::LIST:
                        if (ordinal == 0) { child = &duckdb::ListType::GetChildType(current); }
                        break;
                    case duckdb::LogicalTypeId::ARRAY:
                        if (ordinal == 0) { child = &duckdb::ArrayType::GetChildType(current); }
                        break;
                    case duckdb::LogicalTypeId::MAP:
                        if (ordinal == 0) { child = &duckdb::MapType::KeyType(current); }
                        if (ordinal == 1) { child = &duckdb::MapType::ValueType(current); }
                        break;
                    case duckdb::LogicalTypeId::UNION:
                        if (ordinal < duckdb::UnionType::GetMemberCount(current)) {
                            child = &duckdb::UnionType::GetMemberType(current, ordinal);
                        }
                        break;
                    default: break;
                }
            }
            if (child) {
                ++frame.next_child;
                path.push_back(ordinal);
                pending.push_back({child});
            } else {
                pending.pop_back();
                if (!path.empty()) { path.pop_back(); }
            }
        }
        if (annotations.empty()) { return; }
        if (annotations.size() > std::numeric_limits<size_t>::max() / sizeof(pdd_bridge_type_annotation)) {
            throw std::length_error("Type annotations exceed bridge allocation limit");
        }
        pdd_bridge_annotations result {};
        std::unique_ptr<pdd_bridge_annotations, decltype(&pdd_bridge_annotations_free)>
            owner(&result, pdd_bridge_annotations_free);
        result.entries = static_cast<pdd_bridge_type_annotation *>(
            std::calloc(annotations.size(), sizeof(pdd_bridge_type_annotation)));
        if (!result.entries) { throw std::bad_alloc(); }
        result.count = annotations.size();
        for (size_t i = 0; i < annotations.size(); ++i) {
            const auto &annotation = annotations[i];
            auto &entry = result.entries[i];
            if (!annotation.path.empty()) {
                if (annotation.path.size() > std::numeric_limits<size_t>::max() / sizeof(idx_t)) {
                    throw std::length_error("Type annotation path exceeds bridge allocation limit");
                }
                const auto bytes = annotation.path.size() * sizeof(idx_t);
                entry.path = static_cast<idx_t *>(std::malloc(bytes));
                if (!entry.path) { throw std::bad_alloc(); }
                std::memcpy(entry.path, annotation.path.data(), bytes);
                entry.path_length = annotation.path.size();
            }
            pdd_bridge::bytes(&entry.collation, annotation.collation);
        }
        *out = result;
        owner.release();
    });
}

void pdd_bridge_annotations_free(pdd_bridge_annotations *annotations) {
    if (!annotations) { return; }
    for (idx_t i = 0; i < annotations->count; ++i) {
        std::free(annotations->entries[i].path);
        pdd_bridge_bytes_free(&annotations->entries[i].collation);
    }
    std::free(annotations->entries);
    *annotations = {};
}

pdd_bridge_status pdd_bridge_type_variant(duckdb_logical_type *out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing VARIANT type output");
        *out = reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(duckdb::LogicalType::VARIANT()));
    });
}

pdd_bridge_status pdd_bridge_type_is_tuple(duckdb_logical_type type, bool *out, pdd_bridge_error *error) {
    if (out) { *out = false; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing tuple output");
        *out = pdd_bridge::tuple(pdd_bridge::logical_type(type));
    });
}

pdd_bridge_status pdd_bridge_type_field_name(duckdb_logical_type type, idx_t field, pdd_bridge_bytes *out,
                                            pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] {
        auto &native = pdd_bridge::logical_type(type);
        pdd_bridge::require(out != nullptr, "Missing field name output");
        if (pdd_bridge::structure(native)) {
            pdd_bridge::require(field < duckdb::StructType::GetChildCount(native), "Field index out of bounds");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
            pdd_bridge::bytes(out, duckdb::StructType::GetChildName(native, field).GetIdentifierName());
#else
            pdd_bridge::bytes(out, duckdb::StructType::GetChildName(native, field));
#endif
        } else if (native.id() == duckdb::LogicalTypeId::UNION) {
            pdd_bridge::require(field < duckdb::UnionType::GetMemberCount(native), "Field index out of bounds");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
            pdd_bridge::bytes(out, duckdb::UnionType::GetMemberName(native, field).GetIdentifierName());
#else
            pdd_bridge::bytes(out, duckdb::UnionType::GetMemberName(native, field));
#endif
        } else {
            throw std::invalid_argument("Field names require STRUCT, TUPLE, or UNION");
        }
    });
}

pdd_bridge_status pdd_bridge_value_from_vector(duckdb_vector vector, idx_t row, idx_t row_count,
                                               pdd_bridge_value **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(vector && out, "Missing vector or value output");
        pdd_bridge::require(row < row_count, "Row index out of bounds");
        *out = new pdd_bridge_value(reinterpret_cast<duckdb::Vector *>(vector)->GetValue(row));
    });
}

pdd_bridge_status pdd_bridge_value_from_c(duckdb_value value, pdd_bridge_value **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(value && out, "Missing C value or value output");
        pdd_bridge::validate_type(reinterpret_cast<duckdb::Value *>(value)->type());
        *out = new pdd_bridge_value(*reinterpret_cast<duckdb::Value *>(value));
    });
}

pdd_bridge_status pdd_bridge_value_to_c(const pdd_bridge_value *value, duckdb_value *out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing C value output");
        *out = reinterpret_cast<duckdb_value>(new duckdb::Value(pdd_bridge::value(value)));
    });
}

void pdd_bridge_value_destroy(pdd_bridge_value **value) {
    if (value) {
        delete *value;
        *value = nullptr;
    }
}

pdd_bridge_status pdd_bridge_value_type(const pdd_bridge_value *value, duckdb_logical_type *out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing logical type output");
        *out = reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(pdd_bridge::value(value).type()));
    });
}

pdd_bridge_status pdd_bridge_value_is_null(const pdd_bridge_value *value, bool *out, pdd_bridge_error *error) {
    if (out) { *out = false; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing NULL output");
        *out = pdd_bridge::value(value).IsNull();
    });
}

pdd_bridge_status pdd_bridge_value_render(const pdd_bridge_value *value, duckdb_connection connection,
                                          pdd_bridge_bytes *out, bool *is_null, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    if (is_null) { *is_null = false; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out && is_null, "Missing rendered value or NULL output");
        auto &native = pdd_bridge::value(value);
        *is_null = native.IsNull();
        if (!*is_null) {
            auto rendered = pdd_bridge::cast(native, duckdb::LogicalType::VARCHAR, connection);
            pdd_bridge::bytes(out, duckdb::StringValue::Get(rendered));
        }
    });
}

pdd_bridge_status pdd_bridge_value_string(const pdd_bridge_value *value, pdd_bridge_bytes *out, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] {
        auto &native = pdd_bridge::value(value);
        pdd_bridge::require(!native.IsNull() && (native.type().id() == duckdb::LogicalTypeId::VARCHAR ||
                                                native.type().id() == duckdb::LogicalTypeId::BLOB ||
                                                native.type().id() == duckdb::LogicalTypeId::GEOMETRY),
                            "Raw bytes require a non-NULL VARCHAR, BLOB, or GEOMETRY");
        pdd_bridge::bytes(out, duckdb::StringValue::Get(native));
    });
}

pdd_bridge_status pdd_bridge_value_render_pdo(const pdd_bridge_value *value, duckdb_connection connection,
                                              pdd_bridge_bytes *out, bool *is_null, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    if (is_null) { *is_null = false; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out && is_null, "Missing rendered value or NULL output");
        auto &native = pdd_bridge::value(value);
        *is_null = native.IsNull();
        if (!*is_null) {
            auto compatible = pdd_bridge::geometry_as_hex(native);
            pdd_bridge::bytes(out, pdd_bridge::canonical_pdo(compatible, connection));
        }
    });
}

pdd_bridge_status pdd_bridge_value_child_count(const pdd_bridge_value *value, idx_t *out, pdd_bridge_error *error) {
    if (out) { *out = 0; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing child count output");
        auto &native = pdd_bridge::value(value);
        auto children = pdd_bridge::children(native);
        if (children) {
            *out = children->size();
        } else if (!native.IsNull() && native.type().id() == duckdb::LogicalTypeId::UNION) {
            *out = 2;
        }
    });
}

pdd_bridge_status pdd_bridge_value_child(const pdd_bridge_value *value, idx_t index, pdd_bridge_value **out,
                                         pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing child output");
        auto &native = pdd_bridge::value(value);
        auto children = pdd_bridge::children(native);
        if (children) {
            pdd_bridge::require(index < children->size(), "Child index out of bounds");
            *out = new pdd_bridge_value((*children)[index]);
        } else if (!native.IsNull() && native.type().id() == duckdb::LogicalTypeId::UNION) {
            pdd_bridge::require(index < 2, "Child index out of bounds");
            *out = new pdd_bridge_value(index == 0 ? duckdb::Value::UTINYINT(duckdb::UnionValue::GetTag(native)) :
                                                    duckdb::UnionValue::GetValue(native));
        } else {
            throw std::invalid_argument("Value has no children");
        }
    });
}

pdd_bridge_status pdd_bridge_value_cast(const pdd_bridge_value *value, duckdb_logical_type type,
                                       duckdb_connection connection, pdd_bridge_value **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing cast output");
        *out = new pdd_bridge_value(pdd_bridge::cast(pdd_bridge::value(value), pdd_bridge::logical_type(type), connection));
    });
}

pdd_bridge_status pdd_bridge_value_struct(const pdd_bridge_bytes *names, pdd_bridge_value *const *children,
                                          idx_t count, bool tuple, pdd_bridge_value **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out && (!count || children) && (tuple || !count || names), "Missing STRUCT inputs");
        duckdb::child_list_t<duckdb::Value> fields;
        fields.reserve(count);
        bool has_name = false;
        for (idx_t i = 0; i < count; i++) {
            pdd_bridge::require(tuple || names[i].data || !names[i].length, "Missing field name bytes");
            std::string name = tuple ? std::string() : std::string(names[i].data ? names[i].data : "", names[i].length);
            has_name = has_name || !name.empty();
            fields.emplace_back(std::move(name), pdd_bridge::value(children[i]));
        }
        pdd_bridge::require(tuple || !count || has_name,
                            "STRUCT fields cannot all be unnamed; use explicit VARIANT object construction");
        *out = new pdd_bridge_value(duckdb::Value::STRUCT(std::move(fields)));
    });
}

pdd_bridge_status pdd_bridge_value_list(duckdb_logical_type child_type, pdd_bridge_value *const *children,
                                        idx_t count, pdd_bridge_value **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out && (!count || children), "Missing LIST inputs");
        duckdb::vector<duckdb::Value> values;
        values.reserve(count);
        for (idx_t i = 0; i < count; i++) {
            values.push_back(pdd_bridge::value(children[i]));
        }
        *out = new pdd_bridge_value(duckdb::Value::LIST(pdd_bridge::logical_type(child_type), std::move(values)));
    });
}
}
