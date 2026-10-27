#include "pdo_duckdb_bridge_query.h"
#include "pdo_duckdb_bridge_internal.hpp"

#include "duckdb.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/parser.hpp"
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#endif

#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

struct pdd_bridge_result {
	duckdb::shared_ptr<duckdb::ClientContext> context;
	duckdb::unique_ptr<duckdb::QueryResult> result;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	duckdb::unique_ptr<duckdb::QueryResultStream<>> stream;
#endif
	duckdb::unique_ptr<duckdb::DataChunk> first_chunk;
	duckdb::vector<duckdb::LogicalType> types;
	duckdb::vector<duckdb::string> names;
	idx_t changed = 0;
	bool streaming = false;

	duckdb::unique_ptr<duckdb::DataChunk> Fetch() {
		if (first_chunk) {
			return std::move(first_chunk);
		}
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		if (stream) {
			auto chunk = stream->Fetch();
			if (stream->HasError()) {
				throw std::runtime_error(stream->GetError());
			}
			return chunk;
		}
#endif
		auto chunk = result->Fetch();
		if (result->HasError()) {
			throw std::runtime_error(result->GetError());
		}
		return chunk;
	}
};

namespace {

duckdb::Connection &connection(duckdb_connection handle) {
	pdd_bridge::require(handle != nullptr, "Connection must not be NULL");
	// The version-matched v1 C API represents duckdb_connection as Connection *.
	return *reinterpret_cast<duckdb::Connection *>(handle);
}

duckdb::string query_string(const char *sql, size_t length) {
	pdd_bridge::require(sql != nullptr, "SQL must not be NULL");
	pdd_bridge::require(std::memchr(sql, '\0', length) == nullptr, "SQL must not contain NUL bytes");
	return duckdb::string(sql, length);
}

template <class F>
pdd_bridge_status query_guard(duckdb_connection handle, const char *sql, size_t length,
	pdd_bridge_error *error, F body) noexcept {
	return pdd_bridge::guard(error, [&] {
		auto &conn = connection(handle);
		auto query = query_string(sql, length);
		try {
			body(conn, query);
		} catch (const duckdb::Exception &exception) {
			duckdb::ErrorData formatted(exception);
			conn.context->ProcessError(formatted, query);
			throw std::runtime_error(formatted.Message());
		}
	});
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
struct ParseOverrideMember {
	using Type = duckdb::unique_ptr<duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>>> duckdb::ParseIterator::*;
	friend Type override_statements_member(ParseOverrideMember) noexcept;
};

template <ParseOverrideMember::Type Member>
struct ParseOverrideAccess {
	friend ParseOverrideMember::Type override_statements_member(ParseOverrideMember) noexcept {
		return Member;
	}
};

// Explicit instantiation permits the exact-source member pointer without copying the engine's object layout.
template struct ParseOverrideAccess<&duckdb::ParseIterator::overridden_statements>;

duckdb::AllowParserOverride legacy_parser_mode(duckdb::Connection &conn) {
	bool enabled = false;
	conn.context->TryGetCurrentSetting("enable_legacy_parser", enabled);
	if (!enabled) {
		return duckdb::AllowParserOverride::DEFAULT_OVERRIDE;
	}
	duckdb::string override_setting;
	pdd_bridge::require(conn.context->TryGetCurrentSetting("allow_parser_override_extension", override_setting),
		"Parser override setting is unavailable");
	return duckdb::EnumUtil::FromString<duckdb::AllowParserOverride>(override_setting);
}

[[noreturn]] void throw_extraction_error(duckdb::Connection &conn, const duckdb::string &query,
	const duckdb::Exception &exception) {
	duckdb::ErrorData formatted(exception);
	conn.context->ProcessError(formatted, query);
	// Match duckdb_extract_statements: process, throw, then reconstruct the C API diagnostic.
	try {
		formatted.Throw();
	} catch (const duckdb::Exception &processed) {
		throw std::runtime_error(duckdb::ErrorData(processed).Message());
	}
}
#endif

pdd_bridge_sql_info classify_statement(duckdb::SQLStatement &statement) {
	pdd_bridge_sql_info info = {PDD_BRIDGE_SQL_OTHER, false, statement.named_param_map.size()};
	switch (statement.type) {
	case duckdb::StatementType::SELECT_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_SELECT;
		break;
	case duckdb::StatementType::CREATE_STATEMENT: {
		auto &create = *statement.Cast<duckdb::CreateStatement>().info;
		if (create.type == duckdb::CatalogType::TABLE_ENTRY && create.Cast<duckdb::CreateTableInfo>().query) {
			info.kind = PDD_BRIDGE_SQL_CREATE_TABLE_AS;
		}
		break;
	}
	case duckdb::StatementType::COPY_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_COPY;
		break;
	case duckdb::StatementType::INSERT_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_INSERT;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		info.has_returning = !statement.Cast<duckdb::InsertStatement>().node->returning_list.empty();
#else
		info.has_returning = !statement.Cast<duckdb::InsertStatement>().returning_list.empty();
#endif
		break;
	case duckdb::StatementType::UPDATE_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_UPDATE;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		info.has_returning = !statement.Cast<duckdb::UpdateStatement>().node->returning_list.empty();
#else
		info.has_returning = !statement.Cast<duckdb::UpdateStatement>().returning_list.empty();
#endif
		break;
	case duckdb::StatementType::DELETE_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_DELETE;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		info.has_returning = !statement.Cast<duckdb::DeleteStatement>().node->returning_list.empty();
#else
		info.has_returning = !statement.Cast<duckdb::DeleteStatement>().returning_list.empty();
#endif
		break;
	case duckdb::StatementType::MERGE_INTO_STATEMENT:
		info.kind = PDD_BRIDGE_SQL_MERGE;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		info.has_returning = !statement.Cast<duckdb::MergeIntoStatement>().node->returning_list.empty();
#else
		info.has_returning = !statement.Cast<duckdb::MergeIntoStatement>().returning_list.empty();
#endif
		break;
	default:
		break;
	}
	return info;
}

pdd_bridge_sql_info classify(duckdb::Connection &conn, const duckdb::string &sql) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	conn.context->VerifyNoOpenResult();
#endif
	// CONNECT routes parsing through the remote catalog. A separate context parses only the original SQL.
	duckdb::Connection parser(*conn.context->db);
	parser.context->config = conn.context->config;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	parser.context->config.connected_grammar.reset();
#endif
	duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> statements;
	try {
		statements = parser.ExtractStatements(sql);
		if (statements.empty()) {
			throw duckdb::InvalidInputException("No statement to prepare!");
		}
		if (statements.size() > 1) {
			throw duckdb::InvalidInputException("Cannot prepare multiple statements at once!");
		}
	} catch (...) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		if (conn.context->HasActiveTransaction()) {
			conn.context->RunFunctionInTransaction([] { throw; }, false);
		}
#endif
		throw;
	}
	return classify_statement(*statements[0]);
}

pdd_bridge_transaction_effect transaction_effect(duckdb::SQLStatement &statement) {
	if (statement.type == duckdb::StatementType::EXPLAIN_STATEMENT) {
		auto &explain = statement.Cast<duckdb::ExplainStatement>();
		return explain.explain_type == duckdb::ExplainType::EXPLAIN_ANALYZE ?
			transaction_effect(*explain.stmt) : PDD_BRIDGE_TRANSACTION_NONE;
	}
	if (statement.type != duckdb::StatementType::TRANSACTION_STATEMENT) {
		return PDD_BRIDGE_TRANSACTION_NONE;
	}
	return statement.Cast<duckdb::TransactionStatement>().info->type == duckdb::TransactionType::BEGIN_TRANSACTION ?
		PDD_BRIDGE_TRANSACTION_BEGIN : PDD_BRIDGE_TRANSACTION_END;
}

bool control_statement(duckdb::StatementType type) {
	using Type = duckdb::StatementType;
	switch (type) {
	case Type::CREATE_STATEMENT:
	case Type::ALTER_STATEMENT:
	case Type::TRANSACTION_STATEMENT:
	case Type::ANALYZE_STATEMENT:
	case Type::VARIABLE_SET_STATEMENT:
	case Type::CREATE_FUNC_STATEMENT:
	case Type::DROP_STATEMENT:
	case Type::EXPORT_STATEMENT:
	case Type::VACUUM_STATEMENT:
	case Type::SET_STATEMENT:
	case Type::LOAD_STATEMENT:
	case Type::ATTACH_STATEMENT:
	case Type::DETACH_STATEMENT:
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	case Type::CONNECT_STATEMENT:
	case Type::DISCONNECT_STATEMENT:
#endif
		return true;
	default:
		return false;
	}
}

void read_metadata(pdd_bridge_result &output) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	if (output.stream) {
		if (!output.stream->MetadataAvailable()) {
			output.first_chunk = output.Fetch();
		}
		output.types = output.stream->GetTypes();
		for (auto &name : output.stream->GetNames()) {
			output.names.push_back(name.GetIdentifierName());
		}
	} else {
		output.types = output.result->GetTypes();
		for (auto &name : output.result->GetNames()) {
			output.names.push_back(name.GetIdentifierName());
		}
	}
#else
	output.types = output.result->types;
	output.names = output.result->names;
#endif
}

bool changes_rows(pdd_bridge_sql_info info) {
	return !info.has_returning && (info.kind == PDD_BRIDGE_SQL_INSERT || info.kind == PDD_BRIDGE_SQL_UPDATE ||
		info.kind == PDD_BRIDGE_SQL_DELETE || info.kind == PDD_BRIDGE_SQL_MERGE ||
		info.kind == PDD_BRIDGE_SQL_CREATE_TABLE_AS);
}

bool read_affected_rows(pdd_bridge_result &output, pdd_bridge_sql_info info) {
	bool count_schema = output.types.size() == 1 && output.names[0] == "Count" &&
		output.types[0].id() == duckdb::LogicalTypeId::BIGINT;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	bool native_count = output.result && output.result->GetStatementProperties().return_type ==
		duckdb::StatementReturnType::CHANGED_ROWS;
#else
	bool native_count = output.result && output.result->properties.return_type ==
		duckdb::StatementReturnType::CHANGED_ROWS;
#endif
	if (!native_count && !changes_rows(info) && !(info.kind == PDD_BRIDGE_SQL_COPY && count_schema)) {
		return false;
	}
	pdd_bridge::require(count_schema, "DML returned an invalid affected-row result");
	output.first_chunk = output.Fetch();
	if (output.first_chunk && output.first_chunk->size() != 0) {
		pdd_bridge::require(output.first_chunk->size() == 1, "DML returned multiple affected-row counts");
		auto count = output.first_chunk->GetValue(0, 0);
		pdd_bridge::require(!count.IsNull(), "DML returned a NULL affected-row count");
		auto changed = count.GetValue<int64_t>();
		pdd_bridge::require(changed >= 0, "DML returned a negative affected-row count");
		output.changed = static_cast<idx_t>(changed);
	}
	auto extra = output.result->Fetch();
	if (output.result->HasError()) {
		throw std::runtime_error(output.result->GetError());
	}
	pdd_bridge::require(!extra || extra->size() == 0, "DML returned multiple affected-row counts");
	return true;
}

void finish_batch_result(pdd_bridge_result &output, pdd_bridge_sql_info classification,
	pdd_bridge_transaction_effect effect, bool control, bool force_last_count,
	bool &saw_query_result, pdd_bridge_exec_info &info) {
	if (output.result->HasError()) {
		throw std::runtime_error(output.result->GetError());
	}
	++info.completed_statements;
	if (effect != PDD_BRIDGE_TRANSACTION_NONE) {
		info.last_transaction_effect = effect;
	}
	read_metadata(output);
	bool counted = read_affected_rows(output, classification);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	bool returns_rows = output.result->GetStatementProperties().return_type == duckdb::StatementReturnType::QUERY_RESULT;
#else
	bool returns_rows = output.result->properties.return_type == duckdb::StatementReturnType::QUERY_RESULT;
#endif
	saw_query_result = saw_query_result || (!counted && !control && returns_rows);
	info.rows_changed = force_last_count || !saw_query_result ? output.changed : 0;
}

void execute_batch_statement(duckdb::Connection &conn, duckdb::unique_ptr<duckdb::SQLStatement> statement,
	bool force_last_count, bool &saw_query_result, pdd_bridge_exec_info &info) {
	auto classification = classify_statement(*statement);
	auto effect = transaction_effect(*statement);
	bool control = control_statement(statement->type);
	pdd_bridge_result output;
	bool can_prepare = force_last_count;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
	can_prepare = can_prepare && !conn.context->IsConnected() &&
		conn.context->db->GetDatabaseManager().GetRemoteCatalogCount() == 0;
#endif
	if (can_prepare) {
		auto prepared = conn.Prepare(std::move(statement));
		if (prepared->HasError()) {
			info.prepare_failed = true;
			throw std::runtime_error(prepared->GetError());
		}
		duckdb::vector<duckdb::Value> values;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		output.result = prepared->Execute(values);
#else
		output.result = prepared->Execute(values, false);
#endif
	} else {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		output.result = conn.context->IsConnected() ? conn.Query(statement->query) : conn.Query(std::move(statement));
#else
		output.result = conn.Query(std::move(statement));
#endif
	}
	finish_batch_result(output, classification, effect, control, force_last_count, saw_query_result, info);
}

bool is_sql_parse_error(const duckdb::Exception &exception) {
	switch (duckdb::ErrorData(exception).Type()) {
	case duckdb::ExceptionType::PARSER:
	case duckdb::ExceptionType::SYNTAX:
	case duckdb::ExceptionType::EXPRESSION:
	case duckdb::ExceptionType::INVALID_INPUT:
	case duckdb::ExceptionType::CONVERSION:
	case duckdb::ExceptionType::OUT_OF_RANGE:
	case duckdb::ExceptionType::DECIMAL:
	case duckdb::ExceptionType::INVALID_TYPE:
	case duckdb::ExceptionType::UNKNOWN_TYPE:
	case duckdb::ExceptionType::NOT_IMPLEMENTED:
	case duckdb::ExceptionType::BINDER:
	case duckdb::ExceptionType::INVALID_CONFIGURATION:
	case duckdb::ExceptionType::CATALOG:
	case duckdb::ExceptionType::PARAMETER_NOT_ALLOWED:
		return true;
	default:
		return false;
	}
}

#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
class BatchParser {
public:
	explicit BatchParser(duckdb::Connection &conn, const duckdb::string &sql)
		: source(conn), parser(*conn.context->db), normalized(duckdb::Parser::NormalizeSQLString(sql)),
		  tokens(duckdb::Parser::Tokenize(normalized)), iterator(*parser.context, normalized) {
	}

	duckdb::unique_ptr<duckdb::SQLStatement> Next() {
		parser.context->config = source.context->config;
		parser.context->config.connected_grammar.reset();
		if (!iterator.Peek()) {
			return nullptr;
		}
		auto statement = iterator.GetStatement();
		while (token_cursor < tokens.size() && (tokens[token_cursor].start < consumed ||
			tokens[token_cursor].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT ||
			(tokens[token_cursor].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR &&
			 normalized[tokens[token_cursor].start] == ';'))) {
			++token_cursor;
		}
		pdd_bridge::require(token_cursor < tokens.size(), "Cannot locate parsed batch statement");
		auto statement_tokens = duckdb::Parser::Tokenize(statement->query);
		idx_t first = 0;
		while (first < statement_tokens.size() &&
			(statement_tokens[first].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT ||
			 (statement_tokens[first].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR &&
			  statement->query[statement_tokens[first].start] == ';'))) {
			++first;
		}
		pdd_bridge::require(first < statement_tokens.size() &&
			statement_tokens[first].start <= tokens[token_cursor].start, "Cannot locate parsed statement prefix");
		// Legacy parser overrides retain leading whitespace/comments; PEG statements start at their first token.
		auto start = tokens[token_cursor].start - statement_tokens[first].start;
		pdd_bridge::require(!statement->query.empty() &&
			start >= consumed &&
			normalized.compare(start, statement->query.size(), statement->query) == 0,
			"Parsed batch statement does not match its source");
		consumed = start + statement->query.size();
		return statement;
	}

	duckdb::string Remaining() const {
		return normalized.substr(consumed);
	}

private:
	duckdb::Connection &source;
	duckdb::Connection parser;
	duckdb::string normalized;
	duckdb::vector<duckdb::SimplifiedToken> tokens;
	duckdb::ParseIterator iterator;
	idx_t token_cursor = 0;
	idx_t consumed = 0;
};
#endif

} // namespace

extern "C" pdd_bridge_status pdd_bridge_exec_batch(duckdb_connection handle, const char *sql, size_t length,
	bool force_last_count, pdd_bridge_exec_info *info, pdd_bridge_error *error) {
	if (info) {
		*info = {0, 0, PDD_BRIDGE_TRANSACTION_NONE, false, false};
	}
	return query_guard(handle, sql, length, error, [&](duckdb::Connection &conn, const duckdb::string &query) {
		pdd_bridge::require(info != nullptr, "Batch information output must not be NULL");
		bool saw_query_result = false;
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		auto dispatch_unparsed = [&](const duckdb::string &remaining) {
			pdd_bridge_result output;
			output.result = conn.Query(remaining);
			if (!output.result->HasError()) {
				info->parse_failed = false;
			}
			finish_batch_result(output, {PDD_BRIDGE_SQL_OTHER, false, 0}, PDD_BRIDGE_TRANSACTION_NONE,
				false, force_last_count, saw_query_result, *info);
		};
		std::unique_ptr<BatchParser> parser;
		try {
			parser = std::make_unique<BatchParser>(conn, query);
		} catch (const duckdb::Exception &exception) {
			if (!is_sql_parse_error(exception)) {
				throw;
			}
			info->parse_failed = true;
			if (force_last_count) {
				throw_extraction_error(conn, query, exception);
			}
			dispatch_unparsed(query);
			return;
		}
		duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> eager;
		auto next = [&]() -> duckdb::unique_ptr<duckdb::SQLStatement> {
			try {
				return parser->Next();
			} catch (const duckdb::Exception &exception) {
				if (!is_sql_parse_error(exception)) {
					throw;
				}
				info->parse_failed = true;
				if (force_last_count) {
					throw_extraction_error(conn, query, exception);
				}
				// Never replay completed statements when forwarding a parser failure to a remote transaction.
				dispatch_unparsed(parser->Remaining());
				return nullptr;
			}
		};
		if (force_last_count) {
			while (auto statement = next()) {
				eager.push_back(std::move(statement));
			}
			for (auto &statement : eager) {
				execute_batch_statement(conn, std::move(statement), true, saw_query_result, *info);
			}
		} else {
			while (auto statement = next()) {
				execute_batch_statement(conn, std::move(statement), false, saw_query_result, *info);
			}
		}
#else
		duckdb::Connection parser(*conn.context->db);
		parser.context->config = conn.context->config;
		duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> statements;
		try {
			statements = parser.ExtractStatements(query);
		} catch (const duckdb::Exception &exception) {
			info->parse_failed = is_sql_parse_error(exception);
			throw;
		}
		for (auto &statement : statements) {
			execute_batch_statement(conn, std::move(statement), force_last_count, saw_query_result, *info);
		}
#endif
	});
}

extern "C" pdd_bridge_status pdd_bridge_query_classify(duckdb_connection handle, const char *sql, size_t length,
	pdd_bridge_sql_info *info, pdd_bridge_error *error) {
	if (info) {
		*info = {PDD_BRIDGE_SQL_OTHER, false, 0};
	}
	return query_guard(handle, sql, length, error, [&](duckdb::Connection &conn, const duckdb::string &query) {
		pdd_bridge::require(info != nullptr, "SQL information output must not be NULL");
		*info = classify(conn, query);
	});
}

extern "C" pdd_bridge_status pdd_bridge_connection_is_remote(duckdb_connection handle, bool *connected,
	pdd_bridge_error *error) {
	if (connected) {
		*connected = false;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(connected != nullptr, "Remote status output must not be NULL");
		auto &conn = connection(handle);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		*connected = conn.context->IsConnected();
#else
		(void) conn;
#endif
	});
}

extern "C" pdd_bridge_status pdd_bridge_connection_has_remote_catalogs(duckdb_connection handle, bool *attached,
	pdd_bridge_error *error) {
	if (attached) {
		*attached = false;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(attached != nullptr, "Remote catalog output must not be NULL");
		auto &conn = connection(handle);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		*attached = conn.context->db->GetDatabaseManager().GetRemoteCatalogCount() != 0;
#else
		(void) conn;
#endif
	});
}

extern "C" pdd_bridge_status pdd_bridge_clear_local_writers(duckdb_connection handle, pdd_bridge_error *error) {
	return pdd_bridge::guard(error, [&] {
		auto &context = *connection(handle).context;
		context.RunFunctionInTransaction([&] {
			// Worker tasks read enable_profiler without a lock. Only suppress its client-thread output here.
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
			context.config.profiler_print_format = "no_output";
#else
			context.config.profiler_print_format = duckdb::ProfilerPrintFormat::NO_OUTPUT;
			context.config.emit_profiler_output = false;
#endif
			context.config.profiler_save_location.clear();
			context.client_data->log_query_writer.reset();
		}, false);
	});
}

extern "C" pdd_bridge_status pdd_bridge_connection_uses_legacy_parser(duckdb_connection handle, bool *legacy,
	pdd_bridge_error *error) {
	if (legacy) {
		*legacy = false;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(legacy != nullptr, "Parser mode output must not be NULL");
		auto &conn = connection(handle);
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		*legacy = legacy_parser_mode(conn) != duckdb::AllowParserOverride::DEFAULT_OVERRIDE;
#else
		(void) conn;
		*legacy = true;
#endif
	});
}

extern "C" pdd_bridge_status pdd_bridge_query_uses_legacy_parser(duckdb_connection handle,
	const char *sql, size_t length, bool *legacy, pdd_bridge_error *error) {
	if (legacy) {
		*legacy = false;
	}
	return query_guard(handle, sql, length, error, [&](duckdb::Connection &conn, const duckdb::string &query) {
		pdd_bridge::require(legacy != nullptr, "Parser mode output must not be NULL");
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
		conn.context->VerifyNoOpenResult();
		auto mode = legacy_parser_mode(conn);
		if (mode != duckdb::AllowParserOverride::FALLBACK_OVERRIDE) {
			*legacy = mode == duckdb::AllowParserOverride::STRICT_OVERRIDE;
			return;
		}
		duckdb::Connection parser(*conn.context->db);
		parser.context->config = conn.context->config;
		parser.context->config.connected_grammar.reset();
		std::unique_ptr<duckdb::ParseIterator> iterator;
		try {
			iterator = std::make_unique<duckdb::ParseIterator>(*parser.context, query);
			iterator->Peek();
		} catch (const duckdb::Exception &exception) {
			if (!is_sql_parse_error(exception)) {
				throw;
			}
			// Final classification applies the actual transaction's error policy; this probe only selects grammar.
		}
		*legacy = iterator && ((*iterator).*override_statements_member(ParseOverrideMember())) != nullptr;
#else
		(void) conn;
		(void) query;
		*legacy = true;
#endif
	});
}

extern "C" pdd_bridge_status pdd_bridge_query_execute(duckdb_connection handle, const char *sql, size_t length,
	bool unbuffered, pdd_bridge_result **result, pdd_bridge_error *error) {
	if (result) {
		*result = nullptr;
	}
	return query_guard(handle, sql, length, error, [&](duckdb::Connection &conn, const duckdb::string &query) {
		pdd_bridge::require(result != nullptr, "Result output must not be NULL");
		auto info = classify(conn, query);
		auto output = std::make_unique<pdd_bridge_result>();
		output->context = conn.context;
		if (unbuffered && info.kind == PDD_BRIDGE_SQL_SELECT) {
#if PDO_DUCKDB_BRIDGE_ENGINE_MAJOR >= 2
			auto submitted = conn.Submit(query);
			if (submitted->HasError()) {
				throw std::runtime_error(submitted->GetError());
			}
			if (duckdb::StreamsByDefault(duckdb::StatementType::SELECT_STATEMENT, *submitted, false)) {
				output->stream = duckdb::make_uniq<duckdb::QueryResultStream<>>(std::move(submitted));
				output->streaming = true;
			} else {
				submitted->Materialize();
				output->result = std::move(submitted);
			}
#else
			output->result = conn.SendQuery(query);
			output->streaming = output->result->type == duckdb::QueryResultType::STREAM_RESULT;
#endif
		} else {
			output->result = conn.Query(query);
		}
		if (output->result && output->result->HasError()) {
			throw std::runtime_error(output->result->GetError());
		}
		read_metadata(*output);
		read_affected_rows(*output, info);
		*result = output.release();
	});
}

extern "C" pdd_bridge_status pdd_bridge_result_info(pdd_bridge_result *result, idx_t *column_count,
	idx_t *rows_changed, bool *streaming, pdd_bridge_error *error) {
	if (column_count) {
		*column_count = 0;
	}
	if (rows_changed) {
		*rows_changed = 0;
	}
	if (streaming) {
		*streaming = false;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(result && column_count && rows_changed && streaming, "Result information arguments must not be NULL");
		*column_count = result->types.size();
		*rows_changed = result->changed;
		*streaming = result->streaming;
	});
}

extern "C" pdd_bridge_status pdd_bridge_result_column(pdd_bridge_result *result, idx_t column,
	pdd_bridge_bytes *name, duckdb_logical_type *type, pdd_bridge_error *error) {
	if (name) {
		*name = {nullptr, 0};
	}
	if (type) {
		*type = nullptr;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(result && name && type, "Column metadata arguments must not be NULL");
		pdd_bridge::require(column < result->types.size(), "Column index is out of range");
		auto copy = std::make_unique<duckdb::LogicalType>(result->types[column]);
		pdd_bridge::bytes(name, result->names[column]);
		*type = reinterpret_cast<duckdb_logical_type>(copy.release());
	});
}

extern "C" pdd_bridge_status pdd_bridge_result_fetch(pdd_bridge_result *result, duckdb_data_chunk *chunk,
	pdd_bridge_error *error) {
	if (chunk) {
		*chunk = nullptr;
	}
	return pdd_bridge::guard(error, [&] {
		pdd_bridge::require(result && chunk, "Fetch arguments must not be NULL");
		*chunk = reinterpret_cast<duckdb_data_chunk>(result->Fetch().release());
	});
}

extern "C" void pdd_bridge_result_destroy(pdd_bridge_result **result) {
	if (!result) {
		return;
	}
	try {
		std::unique_ptr<pdd_bridge_result> owner(*result);
		*result = nullptr;
	} catch (...) {
		*result = nullptr;
	}
}
