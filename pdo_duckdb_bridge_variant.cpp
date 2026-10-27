#include "pdo_duckdb_bridge_variant.h"
#include "pdo_duckdb_bridge_internal.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
#include "duckdb/common/types/variant/variant_builder.hpp"
#else
#include "duckdb/common/types/variant_value.hpp"
#endif
#include <map>
#include <limits>
#include <memory>
#include <utility>

namespace {
struct VariantStorage {
    duckdb::Value value;
    std::unique_ptr<duckdb::Vector> vector;
    duckdb::RecursiveUnifiedVectorFormat format;
    std::unique_ptr<duckdb::UnifiedVariantVectorData> data;

    explicit VariantStorage(duckdb::Value input) : value(std::move(input)) {
        pdd_bridge::require(value.type().id() == duckdb::LogicalTypeId::VARIANT, "Expected VARIANT value");
        if (value.IsNull()) {
            return;
        }
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        vector = std::make_unique<duckdb::Vector>(value, duckdb::count_t(1));
        duckdb::Vector::RecursiveToUnifiedFormat(*vector, format);
#else
        vector = std::make_unique<duckdb::Vector>(value);
        duckdb::Vector::RecursiveToUnifiedFormat(*vector, 1, format);
#endif
        data = std::make_unique<duckdb::UnifiedVariantVectorData>(format);
        pdd_bridge::require(data->RowIsValid(0) && data->GetValuesListEntry(0).length != 0,
                            "VARIANT has no root value");
    }
};
}

struct pdd_bridge_variant {
    std::shared_ptr<VariantStorage> storage;
    uint32_t index;

    pdd_bridge_variant(std::shared_ptr<VariantStorage> storage_p, uint32_t index_p)
        : storage(std::move(storage_p)), index(index_p) {}
};

namespace {
const pdd_bridge_variant &node(const pdd_bridge_variant *value) {
    pdd_bridge::require(value != nullptr, "Missing VARIANT node");
    return *value;
}

pdd_bridge_variant_kind kind(const pdd_bridge_variant &value) {
    if (value.storage->value.IsNull()) {
        return PDD_BRIDGE_VARIANT_SQL_NULL;
    }
    switch (value.storage->data->GetTypeId(0, value.index)) {
        case duckdb::VariantLogicalType::VARIANT_NULL: return PDD_BRIDGE_VARIANT_NULL;
        case duckdb::VariantLogicalType::OBJECT: return PDD_BRIDGE_VARIANT_OBJECT;
        case duckdb::VariantLogicalType::ARRAY: return PDD_BRIDGE_VARIANT_ARRAY;
        default: return PDD_BRIDGE_VARIANT_SCALAR;
    }
}

duckdb::VariantNestedData nested(const pdd_bridge_variant &value) {
    auto type = kind(value);
    pdd_bridge::require(type == PDD_BRIDGE_VARIANT_OBJECT || type == PDD_BRIDGE_VARIANT_ARRAY,
                        "VARIANT node is not a container");
    auto result = duckdb::VariantUtils::DecodeNestedData(*value.storage->data, 0, value.index);
    auto children = value.storage->data->GetChildrenListEntry(0);
    pdd_bridge::require(result.children_idx <= children.length &&
                        result.child_count <= children.length - result.children_idx,
                        "VARIANT child range is invalid");
    return result;
}

uint32_t child_slot(const pdd_bridge_variant &value, idx_t index) {
    auto children = nested(value);
    pdd_bridge::require(index < children.child_count, "VARIANT child index out of bounds");
    pdd_bridge::require(index <= std::numeric_limits<uint32_t>::max() - children.children_idx,
                        "VARIANT child index exceeds its storage limit");
    return children.children_idx + static_cast<uint32_t>(index);
}

void from_value(duckdb::Value value, pdd_bridge_variant **out) {
    pdd_bridge::require(out != nullptr, "Missing VARIANT output");
    auto storage = std::make_shared<VariantStorage>(std::move(value));
    *out = std::make_unique<pdd_bridge_variant>(std::move(storage), 0).release();
}

duckdb::Value input_value(duckdb_value value) {
    pdd_bridge::require(value != nullptr, "Missing VARIANT input child");
    auto &native = *reinterpret_cast<duckdb::Value *>(value);
    if (native.IsNull()) {
        return duckdb::Value();
    }
    switch (native.type().id()) {
        case duckdb::LogicalTypeId::LIST:
        case duckdb::LogicalTypeId::ARRAY:
        case duckdb::LogicalTypeId::STRUCT:
        case duckdb::LogicalTypeId::MAP:
        case duckdb::LogicalTypeId::UNION:
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        case duckdb::LogicalTypeId::TUPLE:
#endif
            throw std::invalid_argument("Nested VARIANT input must use explicit object or array construction");
        default: return native;
    }
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
void emit_node(const pdd_bridge_variant &value, duckdb::VariantBuilder &builder, bool geometry_hex = false) {
    switch (kind(value)) {
        case PDD_BRIDGE_VARIANT_SQL_NULL:
        case PDD_BRIDGE_VARIANT_NULL:
            builder.EmitNull();
            break;
        case PDD_BRIDGE_VARIANT_OBJECT: {
            auto children = nested(value);
            builder.EmitObject(children.child_count, [&](idx_t index) {
                auto key = value.storage->data->GetKeysIndex(0, children.children_idx + static_cast<uint32_t>(index));
                return value.storage->data->GetKey(0, key);
            }, [&](idx_t index) {
                auto child = value.storage->data->GetValuesIndex(0, children.children_idx + static_cast<uint32_t>(index));
                emit_node(pdd_bridge_variant(value.storage, child), builder, geometry_hex);
            });
            break;
        }
        case PDD_BRIDGE_VARIANT_ARRAY: {
            auto children = nested(value);
            builder.EmitArray(children.child_count, [&](idx_t index) {
                auto child = value.storage->data->GetValuesIndex(0, children.children_idx + static_cast<uint32_t>(index));
                emit_node(pdd_bridge_variant(value.storage, child), builder, geometry_hex);
            });
            break;
        }
        case PDD_BRIDGE_VARIANT_SCALAR: {
            auto scalar = duckdb::VariantUtils::ConvertVariantToValue(*value.storage->data, 0, value.index);
            if (geometry_hex && scalar.type().id() == duckdb::LogicalTypeId::GEOMETRY) {
                scalar = pdd_bridge::geometry_as_hex(scalar);
            }
            builder.EmitPrimitive(scalar, duckdb::NumericCast<uint32_t>(builder.blob.size()));
            break;
        }
    }
}

void emit_value(const duckdb::Value &value, duckdb::VariantBuilder &builder) {
    if (value.IsNull()) {
        builder.EmitNull();
    } else if (value.type().id() == duckdb::LogicalTypeId::VARIANT) {
        auto storage = std::make_shared<VariantStorage>(value);
        emit_node(pdd_bridge_variant(std::move(storage), 0), builder);
    } else {
        builder.EmitPrimitive(value, duckdb::NumericCast<uint32_t>(builder.blob.size()));
    }
}

struct VariantInput {
    bool object;
    duckdb::vector<duckdb::string> keys;
    duckdb::vector<duckdb::Value> children;

    bool Emit(idx_t, duckdb::VariantBuilder &builder) {
        if (object) {
            builder.EmitObject(children.size(), [&](idx_t index) {
                return duckdb::string_t(keys[index]);
            }, [&](idx_t index) { emit_value(children[index], builder); });
        } else {
            builder.EmitArray(children.size(), [&](idx_t index) { emit_value(children[index], builder); });
        }
        return false;
    }
};
#else
duckdb::VariantValue variant_tree(const pdd_bridge_variant &value, bool geometry_hex = false) {
    auto type = kind(value);
    if (type == PDD_BRIDGE_VARIANT_SQL_NULL || type == PDD_BRIDGE_VARIANT_NULL) {
        return duckdb::VariantValue::NullValue();
    }
    if (type == PDD_BRIDGE_VARIANT_SCALAR) {
        auto scalar = duckdb::VariantUtils::ConvertVariantToValue(*value.storage->data, 0, value.index);
        if (geometry_hex && scalar.type().id() == duckdb::LogicalTypeId::GEOMETRY) {
            scalar = pdd_bridge::geometry_as_hex(scalar);
        }
        return duckdb::VariantValue(std::move(scalar));
    }
    auto children = nested(value);
    duckdb::VariantValue result(type == PDD_BRIDGE_VARIANT_OBJECT ? duckdb::VariantValueType::OBJECT :
                                                                  duckdb::VariantValueType::ARRAY);
    for (idx_t index = 0; index < children.child_count; index++) {
        auto slot = children.children_idx + static_cast<uint32_t>(index);
        auto child = value.storage->data->GetValuesIndex(0, slot);
        auto converted = variant_tree(pdd_bridge_variant(value.storage, child), geometry_hex);
        if (type == PDD_BRIDGE_VARIANT_OBJECT) {
            auto key = value.storage->data->GetKeysIndex(0, slot);
            result.AddChild(value.storage->data->GetKey(0, key).GetString(), std::move(converted));
        } else {
            result.AddItem(std::move(converted));
        }
    }
    return result;
}

duckdb::VariantValue variant_input(duckdb::Value value) {
    if (value.type().id() == duckdb::LogicalTypeId::VARIANT) {
        auto storage = std::make_shared<VariantStorage>(std::move(value));
        return variant_tree(pdd_bridge_variant(std::move(storage), 0));
    }
    return duckdb::VariantValue(std::move(value));
}

void restore_object_order(const std::shared_ptr<VariantStorage> &original, duckdb::Vector &result) {
    auto rebuilt = std::make_shared<VariantStorage>(result.GetValue(0));
    duckdb::VariantVectorData target(result);
    duckdb::vector<std::pair<uint32_t, uint32_t>> pending {{0, 0}};
    while (!pending.empty()) {
        auto indices = pending.back();
        pending.pop_back();
        pdd_bridge_variant before(original, indices.first), after(rebuilt, indices.second);
        auto type = kind(before);
        if (type != PDD_BRIDGE_VARIANT_OBJECT && type != PDD_BRIDGE_VARIANT_ARRAY) {
            continue;
        }
        pdd_bridge::require(kind(after) == type, "Geometry conversion changed VARIANT container kind");
        auto source_children = nested(before), target_children = nested(after);
        pdd_bridge::require(source_children.child_count == target_children.child_count,
                            "Geometry conversion changed VARIANT child count");
        std::map<duckdb::string, uint32_t> members;
        if (type == PDD_BRIDGE_VARIANT_OBJECT) {
            for (uint32_t i = 0; i < target_children.child_count; i++) {
                auto slot = target_children.children_idx + i;
                auto key_index = rebuilt->data->GetKeysIndex(0, slot);
                members.emplace(rebuilt->data->GetKey(0, key_index).GetString(), slot);
            }
        }
        for (uint32_t i = 0; i < source_children.child_count; i++) {
            auto source_slot = source_children.children_idx + i;
            auto target_slot = target_children.children_idx + i;
            auto from_slot = target_slot;
            if (type == PDD_BRIDGE_VARIANT_OBJECT) {
                auto source_key = original->data->GetKeysIndex(0, source_slot);
                auto member = members.find(original->data->GetKey(0, source_key).GetString());
                pdd_bridge::require(member != members.end(), "Geometry conversion changed a VARIANT object key");
                from_slot = member->second;
                auto output_slot = target.children_data[0].offset + target_slot;
                target.keys_index_data[output_slot] = rebuilt->data->GetKeysIndex(0, from_slot);
                target.values_index_data[output_slot] = rebuilt->data->GetValuesIndex(0, from_slot);
                target.keys_index_validity.SetValid(output_slot);
            }
            pending.emplace_back(original->data->GetValuesIndex(0, source_slot),
                                 rebuilt->data->GetValuesIndex(0, from_slot));
        }
    }
}
#endif

void make_variant(const pdd_bridge_bytes *keys, const duckdb_value *children, idx_t count, bool object,
                  duckdb_value *out) {
    pdd_bridge::require(out && (!count || children) && (!object || !count || keys), "Missing VARIANT input");
    pdd_bridge::require(count <= std::numeric_limits<uint32_t>::max(), "VARIANT child count exceeds its storage limit");
    std::map<duckdb::string, idx_t> sorted;
    if (object) {
        for (idx_t index = 0; index < count; index++) {
            pdd_bridge::require(keys[index].data || !keys[index].length, "Missing VARIANT key bytes");
            pdd_bridge::require(keys[index].length <= std::numeric_limits<uint32_t>::max(),
                                "VARIANT key exceeds its storage limit");
            duckdb::string key(keys[index].data ? keys[index].data : "", keys[index].length);
            pdd_bridge::require(sorted.emplace(std::move(key), index).second, "Duplicate VARIANT object key");
        }
    }
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    VariantInput input {object, {}, {}};
    input.children.reserve(count);
    input.keys.reserve(sorted.size());
    if (object) {
        for (auto &entry : sorted) {
            input.keys.push_back(entry.first);
            input.children.push_back(input_value(children[entry.second]));
        }
    } else {
        for (idx_t index = 0; index < count; index++) {
            input.children.push_back(input_value(children[index]));
        }
    }
    duckdb::Vector result(duckdb::LogicalType::VARIANT(), duckdb::capacity_t(1));
    duckdb::BuildVariant(input, 1, result);
#else
    duckdb::VariantValue input(object ? duckdb::VariantValueType::OBJECT : duckdb::VariantValueType::ARRAY);
    if (object) {
        for (auto &entry : sorted) {
            input.AddChild(entry.first, variant_input(input_value(children[entry.second])));
        }
    } else {
        for (idx_t index = 0; index < count; index++) {
            input.AddItem(variant_input(input_value(children[index])));
        }
    }
    duckdb::vector<duckdb::VariantValue> rows;
    rows.push_back(std::move(input));
    duckdb::Vector result(duckdb::LogicalType::VARIANT());
    duckdb::VariantValue::ToVARIANT(rows, result);
#endif
    *out = reinterpret_cast<duckdb_value>(std::make_unique<duckdb::Value>(result.GetValue(0)).release());
}
}

namespace pdd_bridge {
static duckdb::string render_node(const pdd_bridge_variant &value, duckdb_connection connection, bool nested_value) {
    auto type = kind(value);
    if (type == PDD_BRIDGE_VARIANT_SQL_NULL || type == PDD_BRIDGE_VARIANT_NULL) {
        return "NULL";
    }
    if (type == PDD_BRIDGE_VARIANT_SCALAR) {
        auto scalar = duckdb::VariantUtils::ConvertVariantToValue(*value.storage->data, 0, value.index);
        auto rendered = connection ? scalar.CastAs(*reinterpret_cast<duckdb::Connection *>(connection)->context,
                                                   duckdb::LogicalType::VARCHAR) :
                                     scalar.DefaultCastAs(duckdb::LogicalType::VARCHAR);
        auto &text = duckdb::StringValue::Get(rendered);
        return nested_value ? quote_nested(text, false) : text;
    }
    auto children = nested(value);
    bool object = type == PDD_BRIDGE_VARIANT_OBJECT;
    duckdb::string output = object ? "{" : "[";
    for (idx_t i = 0; i < children.child_count; i++) {
        if (i) { output += ", "; }
        auto slot = children.children_idx + static_cast<uint32_t>(i);
        if (object) {
            auto key = value.storage->data->GetKeysIndex(0, slot);
            output += quote_nested(value.storage->data->GetKey(0, key).GetString(), true) + ": ";
        }
        auto child = value.storage->data->GetValuesIndex(0, slot);
        output += render_node(pdd_bridge_variant(value.storage, child), connection, true);
    }
    output += object ? "}" : "]";
    return output;
}

duckdb::string variant_render(const duckdb::Value &value, duckdb_connection connection, bool nested_value) {
    auto storage = std::make_shared<VariantStorage>(value);
    return render_node(pdd_bridge_variant(std::move(storage), 0), connection, nested_value);
}

duckdb::Value variant_geometry_as_hex(const duckdb::Value &value) {
    if (value.IsNull()) {
        return value;
    }
    auto storage = std::make_shared<VariantStorage>(value);
    duckdb::vector<uint32_t> pending {0};
    bool geometry = false;
    while (!pending.empty()) {
        auto index = pending.back();
        pending.pop_back();
        auto type = storage->data->GetTypeId(0, index);
        if (type == duckdb::VariantLogicalType::GEOMETRY) {
            geometry = true;
            break;
        }
        if (type == duckdb::VariantLogicalType::OBJECT || type == duckdb::VariantLogicalType::ARRAY) {
            auto children = nested(pdd_bridge_variant(storage, index));
            for (idx_t i = 0; i < children.child_count; i++) {
                pending.push_back(storage->data->GetValuesIndex(0, children.children_idx + static_cast<uint32_t>(i)));
            }
        }
    }
    if (!geometry) {
        return value;
    }
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
    struct GeometryInput {
        pdd_bridge_variant root;
        bool Emit(idx_t, duckdb::VariantBuilder &builder) {
            emit_node(root, builder, true);
            return false;
        }
    } input {pdd_bridge_variant(std::move(storage), 0)};
    duckdb::Vector result(duckdb::LogicalType::VARIANT(), duckdb::capacity_t(1));
    duckdb::BuildVariant(input, 1, result);
#else
    duckdb::vector<duckdb::VariantValue> rows;
    rows.push_back(variant_tree(pdd_bridge_variant(storage, 0), true));
    duckdb::Vector result(duckdb::LogicalType::VARIANT());
    duckdb::VariantValue::ToVARIANT(rows, result);
    /* The 1.5 builder sorts object maps; preserve the original stored order used by canonical rendering. */
    restore_object_order(storage, result);
#endif
    return result.GetValue(0);
}
}

extern "C" pdd_bridge_status pdd_bridge_variant_from_c(duckdb_value value, pdd_bridge_variant **out,
                                                       pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(value != nullptr, "Missing C value");
        /* The exact-version v1 C API represents duckdb_value as Value *. */
        from_value(*reinterpret_cast<duckdb::Value *>(value), out);
    });
}

extern "C" pdd_bridge_status pdd_bridge_variant_from_vector(duckdb_vector vector, idx_t row, idx_t row_count,
                                                            pdd_bridge_variant **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(vector != nullptr, "Missing vector");
        pdd_bridge::require(row < row_count, "Row index out of bounds");
        from_value(reinterpret_cast<duckdb::Vector *>(vector)->GetValue(row), out);
    });
}

extern "C" void pdd_bridge_variant_destroy(pdd_bridge_variant **value) {
    if (value) {
        std::unique_ptr<pdd_bridge_variant> owned(*value);
        *value = nullptr;
    }
}

extern "C" pdd_bridge_status pdd_bridge_variant_info(const pdd_bridge_variant *value, pdd_bridge_variant_kind *out_kind,
                                                     idx_t *child_count, pdd_bridge_error *error) {
    if (out_kind) { *out_kind = PDD_BRIDGE_VARIANT_SQL_NULL; }
    if (child_count) { *child_count = 0; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out_kind && child_count, "Missing VARIANT information output");
        auto &input = node(value);
        auto type = kind(input);
        if (type == PDD_BRIDGE_VARIANT_OBJECT || type == PDD_BRIDGE_VARIANT_ARRAY) {
            *child_count = nested(input).child_count;
        }
        *out_kind = type;
    });
}

extern "C" pdd_bridge_status pdd_bridge_variant_child(const pdd_bridge_variant *value, idx_t index,
                                                      pdd_bridge_variant **out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing VARIANT child output");
        auto &input = node(value);
        auto slot = child_slot(input, index);
        auto child = input.storage->data->GetValuesIndex(0, slot);
        pdd_bridge::require(child < input.storage->data->GetValuesListEntry(0).length,
                            "VARIANT child value index is invalid");
        *out = std::make_unique<pdd_bridge_variant>(input.storage, child).release();
    });
}

extern "C" pdd_bridge_status pdd_bridge_variant_key(const pdd_bridge_variant *value, idx_t index,
                                                    pdd_bridge_bytes *out, pdd_bridge_error *error) {
    if (out) { *out = {}; }
    return pdd_bridge::guard(error, [&] {
        auto &input = node(value);
        pdd_bridge::require(kind(input) == PDD_BRIDGE_VARIANT_OBJECT, "VARIANT keys require an object");
        auto slot = child_slot(input, index);
        pdd_bridge::require(input.storage->data->KeysIndexIsValid(0, slot), "VARIANT object key is missing");
        auto key = input.storage->data->GetKeysIndex(0, slot);
        pdd_bridge::bytes(out, input.storage->data->GetKey(0, key).GetString());
    });
}

extern "C" pdd_bridge_status pdd_bridge_variant_scalar(const pdd_bridge_variant *value, duckdb_value *out,
                                                       pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] {
        pdd_bridge::require(out != nullptr, "Missing scalar value output");
        auto &input = node(value);
        auto type = kind(input);
        pdd_bridge::require(type != PDD_BRIDGE_VARIANT_OBJECT && type != PDD_BRIDGE_VARIANT_ARRAY,
                            "VARIANT containers require node traversal");
        auto result = type == PDD_BRIDGE_VARIANT_SQL_NULL ? duckdb::Value() :
            duckdb::VariantUtils::ConvertVariantToValue(*input.storage->data, 0, input.index);
        *out = reinterpret_cast<duckdb_value>(std::make_unique<duckdb::Value>(std::move(result)).release());
    });
}

extern "C" pdd_bridge_status pdd_bridge_variant_object(const pdd_bridge_bytes *keys, const duckdb_value *children,
                                                       idx_t count, duckdb_value *out, pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] { make_variant(keys, children, count, true, out); });
}

extern "C" pdd_bridge_status pdd_bridge_variant_array(const duckdb_value *children, idx_t count, duckdb_value *out,
                                                      pdd_bridge_error *error) {
    if (out) { *out = nullptr; }
    return pdd_bridge::guard(error, [&] { make_variant(nullptr, children, count, false, out); });
}
