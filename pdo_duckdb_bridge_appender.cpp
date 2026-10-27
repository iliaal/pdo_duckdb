/*
 * Portions derived from DuckDB src/main/appender.cpp.
 * Copyright 2018-2026 Stichting DuckDB Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "pdo_duckdb_bridge_appender.h"
#include "pdo_duckdb_bridge_internal.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include <memory>

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#endif

namespace {

std::string identifier(pdd_bridge_bytes value) {
    pdd_bridge::require(value.data != nullptr && value.length != 0, "Appender identifiers must not be empty");
    pdd_bridge::require(std::memchr(value.data, '\0', value.length) == nullptr,
                        "Appender identifiers must not contain NUL bytes");
    return std::string(value.data, value.length);
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
class NestedAppender final : public duckdb::BaseAppender {
public:
    NestedAppender(duckdb::Connection &connection, duckdb::QualifiedName name)
        : BaseAppender(duckdb::Allocator::DefaultAllocator(), duckdb::AppenderType::LOGICAL),
          context(connection.context), qualified_name(std::move(name)) {
        auto &client = *connection.context;
        auto binder = duckdb::Binder::CreateBinder(client);
        client.RunFunctionInTransaction([&]() {
            auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(client, qualified_name);
            if (table.ParentCatalog().GetAttached().IsReadOnly()) {
                throw duckdb::InvalidInputException("Cannot append to a readonly database");
            }
            for (auto &column_definition : table.GetColumns().Logical()) {
                columns.push_back(column_definition.Copy());
                if (column_definition.Generated()) {
                    continue;
                }
                const auto index = columns.size() - 1;
                writable_columns.push_back(index);
                types.push_back(column_definition.Type());
                if (!column_definition.HasDefaultValue()) {
                    defaults.emplace(index, duckdb::Value(column_definition.Type()));
                    continue;
                }
                auto expression = column_definition.DefaultValue().Copy();
                duckdb::ConstantBinder default_binder(*binder, client, "DEFAULT value");
                default_binder.target_type = column_definition.Type();
                auto bound = default_binder.Bind(expression);
                if (bound->IsFoldable()) {
                    duckdb::Value value;
                    if (duckdb::ExpressionExecutor::TryEvaluateScalar(client, *bound, value)) {
                        defaults.emplace(index, std::move(value));
                    }
                }
            }
        });
        InitializeChunk();
        collection = duckdb::make_uniq<duckdb::ColumnDataCollection>(allocator, GetActiveTypes());
    }

    ~NestedAppender() override {
        Destructor();
    }

    void AddColumn(const duckdb::Identifier &name) override {
        Flush();
        for (duckdb::idx_t index = 0; index < columns.size(); ++index) {
            if (columns[index].Name() != name) {
                continue;
            }
            if (columns[index].Generated()) {
                throw duckdb::InvalidInputException("Cannot add a generated column to the appender");
            }
            for (auto selected : selected_columns) {
                if (selected == index) {
                    throw duckdb::InvalidInputException("Cannot add the same column twice");
                }
            }
            active_types.push_back(columns[index].Type());
            selected_columns.push_back(index);
            InitializeChunk();
            collection = duckdb::make_uniq<duckdb::ColumnDataCollection>(allocator, GetActiveTypes());
            return;
        }
        throw duckdb::InvalidInputException("The column must exist in the table");
    }

    void ClearColumns() override {
        Flush();
        selected_columns.clear();
        active_types.clear();
        InitializeChunk();
        collection = duckdb::make_uniq<duckdb::ColumnDataCollection>(allocator, GetActiveTypes());
    }

    void AppendDefault() override {
        Append(default_value(column));
    }

    void AppendDefault(duckdb::DataChunk &target, duckdb::idx_t col, duckdb::idx_t row) override {
        auto value = default_value(col);
        Append(target, value, col, row);
    }

protected:
    void FlushInternal(duckdb::ColumnDataCollection &data) override {
        auto client = context.lock();
        if (!client) {
            throw duckdb::InvalidInputException("Appender: Attempting to flush data to a closed connection");
        }
        duckdb::vector<duckdb::Identifier> names;
        for (auto selected : selected_columns) {
            names.push_back(columns[selected].Name());
        }
        auto select = duckdb::make_uniq<duckdb::SelectNode>();
        select->select_list.push_back(duckdb::make_uniq<duckdb::StarExpression>());
        select->from_table = GetColumnDataTableRef(data, "__pdo_duckdb_appended_data", names);
        auto statement = duckdb::make_uniq<duckdb::InsertStatement>();
        statement->node->qualified_name = qualified_name;
        statement->node->columns = std::move(names);
        statement->node->select_statement = duckdb::make_uniq<duckdb::SelectStatement>();
        statement->node->select_statement->node = std::move(select);
        client->Append(std::move(statement));
    }

private:
    duckdb::Value default_value(duckdb::idx_t col) {
        const auto &selection = selected_columns.empty() ? writable_columns : selected_columns;
        if (col >= selection.size()) {
            throw duckdb::InvalidInputException("Column index out of bounds");
        }
        auto found = defaults.find(selection[col]);
        if (found == defaults.end()) {
            throw duckdb::NotImplementedException("AppendDefault requires a foldable default expression");
        }
        return found->second;
    }

    duckdb::weak_ptr<duckdb::ClientContext> context;
    duckdb::QualifiedName qualified_name;
    duckdb::vector<duckdb::ColumnDefinition> columns;
    duckdb::vector<duckdb::idx_t> writable_columns;
    duckdb::vector<duckdb::idx_t> selected_columns;
    duckdb::unordered_map<duckdb::idx_t, duckdb::Value> defaults;
};
#endif

}

extern "C" pdd_bridge_status pdd_bridge_appender_create(duckdb_connection connection,
    const pdd_bridge_bytes *schema_segments, size_t schema_count, pdd_bridge_bytes table,
    duckdb_appender *out, pdd_bridge_error *error) {
    if (out) {
        *out = nullptr;
    }
    return pdd_bridge::guard(error, [&]() {
        pdd_bridge::require(connection != nullptr && out != nullptr, "Missing Appender connection or output");
        pdd_bridge::require(schema_count == 0 || schema_segments != nullptr, "Missing Appender schema segments");
        auto table_name = identifier(table);
        auto &native = *reinterpret_cast<duckdb::Connection *>(connection);
        auto wrapper = std::make_unique<duckdb::AppenderWrapper>();
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
        if (schema_count > 1) {
            duckdb::vector<duckdb::Identifier> path;
            path.reserve(schema_count);
            for (size_t index = 0; index < schema_count; ++index) {
                path.emplace_back(identifier(schema_segments[index]));
            }
            auto qualified = duckdb::QualifiedName::FromCatalogSchema(
                duckdb::DatabaseManager::GetDefaultDatabase(*native.context), std::move(path),
                duckdb::Identifier(std::move(table_name)));
            wrapper->appender = duckdb::make_uniq<NestedAppender>(native, std::move(qualified));
        } else {
            auto schema = schema_count ? identifier(schema_segments[0]) : std::string(DEFAULT_SCHEMA);
            wrapper->appender = duckdb::make_uniq<duckdb::Appender>(native, duckdb::Identifier(schema),
                                                                 duckdb::Identifier(table_name));
        }
#else
        pdd_bridge::require(schema_count <= 1, "Nested schemas require DuckDB 2.0 or newer");
        auto schema = schema_count ? identifier(schema_segments[0]) : std::string(DEFAULT_SCHEMA);
        wrapper->appender = duckdb::make_uniq<duckdb::Appender>(native, schema, table_name);
#endif
        /* Exact v1 appender-c.cpp owns AppenderWrapper and destroys its BaseAppender polymorphically. */
        *out = reinterpret_cast<duckdb_appender>(wrapper.release());
    });
}
