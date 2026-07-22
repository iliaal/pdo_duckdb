/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2026, Ilia Alshanetsky                                 |
  | Copyright (c) 2026, Advanced Internet Designs Inc.                   |
  +----------------------------------------------------------------------+
  | This source file is subject to the BSD 3-Clause license that is      |
  | bundled with this package in the file LICENSE.                       |
  +----------------------------------------------------------------------+
  | Author: Ilia Alshanetsky <ilia@ilia.ws>                              |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"

int _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *msg, const char *file, int line) /* {{{ */
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_error_type *pdo_err = stmt ? &stmt->error_code : &dbh->error_code;
	pdo_duckdb_error_info *einfo;
	bool persistent = dbh->is_persistent;
	const char *errmsg = (msg && *msg) ? msg : "DuckDB operation failed";

	if (stmt && stmt->driver_data) {
		pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
		einfo = &S->einfo;
		persistent = false;
	} else {
		einfo = &H->einfo;
	}

	einfo->file = file;
	einfo->line = line;

	einfo->errcode = 1;
	if (einfo->errmsg) {
		pefree(einfo->errmsg, persistent);
	}
	einfo->errmsg = pestrdup(errmsg, persistent);
	strncpy(*pdo_err, "HY000", sizeof(*pdo_err));

	if (!dbh->methods) {
		pdo_throw_exception(einfo->errcode, einfo->errmsg, pdo_err);
	}

	return einfo->errcode;
}
/* }}} */

static void pdo_duckdb_fetch_error_func(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_error_info *einfo = &H->einfo;

	if (stmt && stmt->driver_data) {
		pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
		einfo = &S->einfo;
	}

	if (einfo->errcode) {
		add_next_index_long(info, einfo->errcode);
		add_next_index_string(info, einfo->errmsg);
	}
}

static void duckdb_handle_closer(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;

	if (H) {
		if (H->conn) {
			duckdb_disconnect(&H->conn);
			H->conn = NULL;
		}
		if (H->db) {
			duckdb_close(&H->db);
			H->db = NULL;
		}
		if (H->einfo.errmsg) {
			pefree(H->einfo.errmsg, dbh->is_persistent);
			H->einfo.errmsg = NULL;
		}
		pefree(H, dbh->is_persistent);
		dbh->driver_data = NULL;
	}
}
/* }}} */

enum {
	PDO_DUCKDB_SQL_DIRECT_TRANSACTION = 1 << 0,
	PDO_DUCKDB_SQL_EXPLAIN_ANALYZE_TRANSACTION = 1 << 1,
	PDO_DUCKDB_SQL_MULTIPLE_STATEMENTS = 1 << 2
};

static unsigned int pdo_duckdb_sql_transaction_flags(const char *sql, size_t len,
	pdo_duckdb_transaction_effect *effects, size_t effects_capacity,
	size_t *statement_count);
static pdo_duckdb_transaction_effect pdo_duckdb_sql_first_transaction_effect(
	const char *sql, size_t len);

static size_t pdo_duckdb_sql_unicode_space_len(const char *sql, size_t len)
{
	const unsigned char *s = (const unsigned char *)sql;

	if (len >= 2 && s[0] == 0xC2 && s[1] == 0xA0) {
		return 2; /* NO-BREAK SPACE */
	}
	if (len >= 3) {
		if (s[0] == 0xE2 && s[1] == 0x80 &&
				((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xAF)) {
			return 3; /* U+2000..U+200A, NARROW NO-BREAK SPACE */
		}
		if ((s[0] == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) ||
				(s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) ||
				(s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)) {
			return 3; /* MEDIUM MATHEMATICAL SPACE, IDEOGRAPHIC SPACE, BOM */
		}
	}
	return 0;
}

static bool pdo_duckdb_sql_may_have_multiple_statements(const char *sql, size_t len)
{
	const char *semicolon = memchr(sql, ';', len);
	size_t pos;

	if (!semicolon) {
		return false;
	}
	pos = (size_t)(semicolon - sql) + 1;
	while (pos < len) {
		unsigned char c = (unsigned char)sql[pos];
		size_t unicode_space_len = pdo_duckdb_sql_unicode_space_len(sql + pos, len - pos);
		if (unicode_space_len) {
			pos += unicode_space_len;
			continue;
		}
		if (c != ';' && c != ' ' && c != '\t' && c != '\n' &&
				c != '\r' && c != '\f' && c != '\v') {
			return true;
		}
		pos++;
	}
	return false;
}

static bool duckdb_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, pdo_stmt_t *stmt, zval *driver_options)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_stmt *S;
	zend_string *rewritten = NULL;
	pdo_duckdb_transaction_effect transaction_effect = PDO_DUCKDB_TRANSACTION_NONE;
	int parse_ret;

	if (PDO_CURSOR_FWDONLY != pdo_attr_lval(driver_options, PDO_ATTR_CURSOR, PDO_CURSOR_FWDONLY)) {
		pdo_duckdb_error(dbh, "DuckDB PDO driver only supports forward-only cursors");
		return false;
	}

	/* duckdb_prepare() takes a NUL-terminated const char*, so an embedded NUL
	 * would silently truncate the statement (e.g. "SELECT 1\0DROP ..." prepares
	 * as "SELECT 1"). Reject it rather than run a different query than intended. */
	if (zend_str_has_nul_byte(sql)) {
		pdo_duckdb_error(dbh, "SQL statement contains a NUL byte");
		return false;
	}

	transaction_effect = pdo_duckdb_sql_first_transaction_effect(
		ZSTR_VAL(sql), ZSTR_LEN(sql));

	if (!pdo_duckdb_enforce_sandbox(H)) {
		pdo_duckdb_error(dbh, "Unable to apply the open_basedir sandbox profile to DuckDB");
		return false;
	}

	S = ecalloc(1, sizeof(pdo_duckdb_stmt));
	S->H = H;
	S->transaction_effect = transaction_effect;
	stmt->driver_data = S;
	stmt->methods = &duckdb_stmt_methods;
	/* DuckDB understands $1/$2 numbered parameters. Declaring NAMED support with
	 * the "$%d" rewrite template makes PDO rewrite BOTH positional '?' and named
	 * ':name' placeholders to $N — and crucially coalesces a repeated ':name'
	 * onto a single $N (the pgsql model), so `WHERE a = :x OR b = :x` works. */
	stmt->supports_placeholders = PDO_PLACEHOLDER_NAMED;
	stmt->named_rewrite_template = "$%d";

	parse_ret = pdo_parse_params(stmt, sql, &rewritten);
	if (parse_ret == 1) {
		/* the query was rewritten (e.g. :name -> ?) */
		sql = rewritten;
	} else if (parse_ret == -1) {
		/* parse failure; pdo_parse_params already set stmt->error_code */
		strncpy(dbh->error_code, stmt->error_code, sizeof(dbh->error_code));
		return false;
	}

	if (duckdb_prepare(H->conn, ZSTR_VAL(sql), &S->prepared) != DuckDBSuccess) {
		pdo_duckdb_error(dbh, duckdb_prepare_error(S->prepared));
		if (S->prepared) {
			duckdb_destroy_prepare(&S->prepared);
			S->prepared = NULL;
		}
		if (rewritten) {
			zend_string_release(rewritten);
		}
		return false;
	}
	S->statement_type = duckdb_prepared_statement_type(S->prepared);

	if (rewritten) {
		zend_string_release(rewritten);
	}
	return true;
}

void pdo_duckdb_sync_transaction_state(pdo_dbh_t *dbh, duckdb_statement_type type,
		pdo_duckdb_transaction_effect effect)
{
	if (effect == PDO_DUCKDB_TRANSACTION_OPEN) {
		dbh->in_txn = true;
		return;
	}
	if (effect == PDO_DUCKDB_TRANSACTION_CLOSE) {
		dbh->in_txn = false;
		return;
	}
	if (type == DUCKDB_STATEMENT_TYPE_TRANSACTION) {
		dbh->in_txn = !dbh->in_txn;
	}
}

static bool pdo_duckdb_ascii_keyword(const char *token, size_t len, const char *keyword)
{
	size_t i;

	if (strlen(keyword) != len) {
		return false;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)token[i];
		if (c >= 'a' && c <= 'z') {
			c = (unsigned char)(c - ('a' - 'A'));
		}
		if (c != (unsigned char)keyword[i]) {
			return false;
		}
	}
	return true;
}

typedef enum {
	PDO_DUCKDB_SQL_TOKEN_EOF,
	PDO_DUCKDB_SQL_TOKEN_WORD,
	PDO_DUCKDB_SQL_TOKEN_QUOTED_WORD,
	PDO_DUCKDB_SQL_TOKEN_LPAREN,
	PDO_DUCKDB_SQL_TOKEN_RPAREN,
	PDO_DUCKDB_SQL_TOKEN_COMMA,
	PDO_DUCKDB_SQL_TOKEN_SEMICOLON,
	PDO_DUCKDB_SQL_TOKEN_OTHER
} pdo_duckdb_sql_token_kind;

typedef struct {
	pdo_duckdb_sql_token_kind kind;
	const char *start;
	size_t len;
} pdo_duckdb_sql_token;

typedef struct {
	const char *sql;
	size_t len;
	size_t pos;
} pdo_duckdb_sql_scanner;

static bool pdo_duckdb_sql_word_start(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool pdo_duckdb_sql_word_continue(unsigned char c)
{
	return pdo_duckdb_sql_word_start(c) || (c >= '0' && c <= '9') || c == '$';
}

static bool pdo_duckdb_sql_skip_dollar_quote(pdo_duckdb_sql_scanner *scanner)
{
	size_t start = scanner->pos;
	size_t tag_end = start + 1;
	size_t delimiter_len;
	size_t i;

	while (tag_end < scanner->len) {
		unsigned char c = (unsigned char)scanner->sql[tag_end];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '_')) {
			break;
		}
		tag_end++;
	}
	if (tag_end >= scanner->len || scanner->sql[tag_end] != '$') {
		return false;
	}

	delimiter_len = tag_end - start + 1;
	i = tag_end + 1;
	while (i <= scanner->len - delimiter_len) {
		if (scanner->sql[i] == '$' &&
				memcmp(scanner->sql + i, scanner->sql + start, delimiter_len) == 0) {
			scanner->pos = i + delimiter_len;
			return true;
		}
		i++;
	}

	/* Let DuckDB report the unterminated literal; consume it here so tokens in
	 * its body cannot be mistaken for statement boundaries. */
	scanner->pos = scanner->len;
	return true;
}

static void pdo_duckdb_sql_skip_quoted(pdo_duckdb_sql_scanner *scanner,
		unsigned char quote, bool backslash_escapes)
{
	scanner->pos++;
	while (scanner->pos < scanner->len) {
		unsigned char c = (unsigned char)scanner->sql[scanner->pos];

		if (backslash_escapes && c == '\\' && scanner->pos + 1 < scanner->len) {
			scanner->pos += 2;
			continue;
		}
		if (c == quote) {
			if (scanner->pos + 1 < scanner->len &&
					(unsigned char)scanner->sql[scanner->pos + 1] == quote) {
				scanner->pos += 2;
				continue;
			}
			scanner->pos++;
			return;
		}
		scanner->pos++;
	}
}

static pdo_duckdb_sql_token pdo_duckdb_sql_next_token(pdo_duckdb_sql_scanner *scanner)
{
	pdo_duckdb_sql_token token = {PDO_DUCKDB_SQL_TOKEN_EOF, NULL, 0};

	while (scanner->pos < scanner->len) {
		unsigned char c = (unsigned char)scanner->sql[scanner->pos];
		size_t unicode_space_len = pdo_duckdb_sql_unicode_space_len(
			scanner->sql + scanner->pos, scanner->len - scanner->pos);

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			scanner->pos++;
			continue;
		}
		if (unicode_space_len) {
			scanner->pos += unicode_space_len;
			continue;
		}
		if (c == '-' && scanner->pos + 1 < scanner->len &&
				scanner->sql[scanner->pos + 1] == '-') {
			scanner->pos += 2;
			while (scanner->pos < scanner->len &&
					scanner->sql[scanner->pos] != '\n' && scanner->sql[scanner->pos] != '\r') {
				scanner->pos++;
			}
			continue;
		}
		if (c == '/' && scanner->pos + 1 < scanner->len &&
				scanner->sql[scanner->pos + 1] == '*') {
			size_t depth = 1;
			scanner->pos += 2;
			while (scanner->pos < scanner->len && depth > 0) {
				if (scanner->pos + 1 < scanner->len &&
						scanner->sql[scanner->pos] == '/' && scanner->sql[scanner->pos + 1] == '*') {
					depth++;
					scanner->pos += 2;
				} else if (scanner->pos + 1 < scanner->len &&
						scanner->sql[scanner->pos] == '*' && scanner->sql[scanner->pos + 1] == '/') {
					depth--;
					scanner->pos += 2;
				} else {
					scanner->pos++;
				}
			}
			continue;
		}
		if ((c == 'E' || c == 'e') && scanner->pos + 1 < scanner->len &&
				scanner->sql[scanner->pos + 1] == '\'') {
			scanner->pos++;
			pdo_duckdb_sql_skip_quoted(scanner, '\'', true);
			token.kind = PDO_DUCKDB_SQL_TOKEN_OTHER;
			return token;
		}
		if (c == '\'') {
			pdo_duckdb_sql_skip_quoted(scanner, c, false);
			token.kind = PDO_DUCKDB_SQL_TOKEN_OTHER;
			return token;
		}
		if (c == '"') {
			size_t start;
			scanner->pos++;
			start = scanner->pos;
			while (scanner->pos < scanner->len) {
				if (scanner->sql[scanner->pos] == '"') {
					if (scanner->pos + 1 < scanner->len && scanner->sql[scanner->pos + 1] == '"') {
						scanner->pos += 2;
						continue;
					}
					token.kind = PDO_DUCKDB_SQL_TOKEN_QUOTED_WORD;
					token.start = scanner->sql + start;
					token.len = scanner->pos - start;
					scanner->pos++;
					return token;
				}
				scanner->pos++;
			}
			token.kind = PDO_DUCKDB_SQL_TOKEN_OTHER;
			return token;
		}
		if (c == '$' && pdo_duckdb_sql_skip_dollar_quote(scanner)) {
			token.kind = PDO_DUCKDB_SQL_TOKEN_OTHER;
			return token;
		}
		if (pdo_duckdb_sql_word_start(c)) {
			size_t start = scanner->pos++;
			while (scanner->pos < scanner->len &&
					pdo_duckdb_sql_word_continue((unsigned char)scanner->sql[scanner->pos])) {
				scanner->pos++;
			}
			token.kind = PDO_DUCKDB_SQL_TOKEN_WORD;
			token.start = scanner->sql + start;
			token.len = scanner->pos - start;
			return token;
		}

		scanner->pos++;
		switch (c) {
			case '(': token.kind = PDO_DUCKDB_SQL_TOKEN_LPAREN; break;
			case ')': token.kind = PDO_DUCKDB_SQL_TOKEN_RPAREN; break;
			case ',': token.kind = PDO_DUCKDB_SQL_TOKEN_COMMA; break;
			case ';': token.kind = PDO_DUCKDB_SQL_TOKEN_SEMICOLON; break;
			default: token.kind = PDO_DUCKDB_SQL_TOKEN_OTHER; break;
		}
		return token;
	}
	return token;
}

static bool pdo_duckdb_sql_token_is_keyword(pdo_duckdb_sql_token token,
		const char *keyword, bool allow_quoted)
{
	return (token.kind == PDO_DUCKDB_SQL_TOKEN_WORD ||
			(allow_quoted && token.kind == PDO_DUCKDB_SQL_TOKEN_QUOTED_WORD)) &&
		pdo_duckdb_ascii_keyword(token.start, token.len, keyword);
}

static pdo_duckdb_transaction_effect pdo_duckdb_sql_token_transaction_effect(
		pdo_duckdb_sql_token token)
{
	if (pdo_duckdb_sql_token_is_keyword(token, "BEGIN", false) ||
			pdo_duckdb_sql_token_is_keyword(token, "START", false)) {
		return PDO_DUCKDB_TRANSACTION_OPEN;
	}
	if (pdo_duckdb_sql_token_is_keyword(token, "COMMIT", false) ||
			pdo_duckdb_sql_token_is_keyword(token, "END", false) ||
			pdo_duckdb_sql_token_is_keyword(token, "ROLLBACK", false) ||
			pdo_duckdb_sql_token_is_keyword(token, "ABORT", false)) {
		return PDO_DUCKDB_TRANSACTION_CLOSE;
	}
	return PDO_DUCKDB_TRANSACTION_NONE;
}

static pdo_duckdb_sql_token pdo_duckdb_sql_scan_explain(
		pdo_duckdb_sql_scanner *scanner, bool *analyze)
{
	pdo_duckdb_sql_token token = pdo_duckdb_sql_next_token(scanner);

	if (pdo_duckdb_sql_token_is_keyword(token, "ANALYZE", false) ||
			pdo_duckdb_sql_token_is_keyword(token, "ANALYSE", false)) {
		*analyze = true;
		token = pdo_duckdb_sql_next_token(scanner);
	}

	if (token.kind == PDO_DUCKDB_SQL_TOKEN_LPAREN) {
		size_t depth = 1;
		bool option_start = true;

		while (depth > 0) {
			token = pdo_duckdb_sql_next_token(scanner);
			if (token.kind == PDO_DUCKDB_SQL_TOKEN_EOF ||
					token.kind == PDO_DUCKDB_SQL_TOKEN_SEMICOLON) {
				return token;
			}
			if (token.kind == PDO_DUCKDB_SQL_TOKEN_LPAREN) {
				depth++;
				continue;
			}
			if (token.kind == PDO_DUCKDB_SQL_TOKEN_RPAREN) {
				depth--;
				continue;
			}
			if (depth == 1 && token.kind == PDO_DUCKDB_SQL_TOKEN_COMMA) {
				option_start = true;
				continue;
			}
			if (depth == 1 && option_start) {
				if (pdo_duckdb_sql_token_is_keyword(token, "ANALYZE", true) ||
						pdo_duckdb_sql_token_is_keyword(token, "ANALYSE", true)) {
					*analyze = true;
				}
				option_start = false;
			}
		}
		token = pdo_duckdb_sql_next_token(scanner);
	}

	return token;
}

static void pdo_duckdb_sql_skip_statement(pdo_duckdb_sql_scanner *scanner, size_t depth)
{
	for (;;) {
		pdo_duckdb_sql_token token = pdo_duckdb_sql_next_token(scanner);

		if (token.kind == PDO_DUCKDB_SQL_TOKEN_EOF) {
			return;
		}
		if (token.kind == PDO_DUCKDB_SQL_TOKEN_LPAREN) {
			depth++;
		} else if (token.kind == PDO_DUCKDB_SQL_TOKEN_RPAREN) {
			if (depth > 0) {
				depth--;
			}
		} else if (token.kind == PDO_DUCKDB_SQL_TOKEN_SEMICOLON && depth == 0) {
			return;
		}
	}
}

static pdo_duckdb_transaction_effect pdo_duckdb_sql_first_transaction_effect(
		const char *sql, size_t len)
{
	pdo_duckdb_sql_scanner scanner = {sql, len, 0};
	pdo_duckdb_sql_token first;
	pdo_duckdb_transaction_effect effect;

	do {
		first = pdo_duckdb_sql_next_token(&scanner);
	} while (first.kind == PDO_DUCKDB_SQL_TOKEN_SEMICOLON);

	effect = pdo_duckdb_sql_token_transaction_effect(first);
	if (effect != PDO_DUCKDB_TRANSACTION_NONE) {
		return effect;
	}
	if (pdo_duckdb_sql_token_is_keyword(first, "EXPLAIN", false)) {
		bool analyze = false;
		pdo_duckdb_sql_token wrapped = pdo_duckdb_sql_scan_explain(&scanner, &analyze);

		if (analyze) {
			return pdo_duckdb_sql_token_transaction_effect(wrapped);
		}
	}
	return PDO_DUCKDB_TRANSACTION_NONE;
}

static unsigned int pdo_duckdb_sql_transaction_flags(const char *sql, size_t len,
		pdo_duckdb_transaction_effect *effects, size_t effects_capacity,
		size_t *statement_count_out)
{
	pdo_duckdb_sql_scanner scanner = {sql, len, 0};
	unsigned int flags = 0;
	size_t statement_count = 0;

	for (;;) {
		pdo_duckdb_sql_token first = pdo_duckdb_sql_next_token(&scanner);
		size_t initial_depth = first.kind == PDO_DUCKDB_SQL_TOKEN_LPAREN ? 1 : 0;
		pdo_duckdb_transaction_effect effect = PDO_DUCKDB_TRANSACTION_NONE;

		if (first.kind == PDO_DUCKDB_SQL_TOKEN_EOF) {
			*statement_count_out = statement_count;
			return flags;
		}
		if (first.kind == PDO_DUCKDB_SQL_TOKEN_SEMICOLON) {
			continue;
		}
		statement_count++;
		if (statement_count > 1) {
			flags |= PDO_DUCKDB_SQL_MULTIPLE_STATEMENTS;
		}
		effect = pdo_duckdb_sql_token_transaction_effect(first);
		if (effect != PDO_DUCKDB_TRANSACTION_NONE) {
			flags |= PDO_DUCKDB_SQL_DIRECT_TRANSACTION;
		} else if (pdo_duckdb_sql_token_is_keyword(first, "EXPLAIN", false)) {
			bool analyze = false;
			pdo_duckdb_sql_token wrapped = pdo_duckdb_sql_scan_explain(&scanner, &analyze);

			if (analyze) {
				effect = pdo_duckdb_sql_token_transaction_effect(wrapped);
				if (effect != PDO_DUCKDB_TRANSACTION_NONE) {
					flags |= PDO_DUCKDB_SQL_EXPLAIN_ANALYZE_TRANSACTION;
				}
			}
			initial_depth = wrapped.kind == PDO_DUCKDB_SQL_TOKEN_LPAREN ? 1 : 0;
		}
		if (effects && statement_count <= effects_capacity) {
			effects[statement_count - 1] = effect;
		}
		pdo_duckdb_sql_skip_statement(&scanner, initial_depth);
	}
}

static zend_long pdo_duckdb_exec_transaction_multi(pdo_dbh_t *dbh,
		duckdb_extracted_statements extracted, idx_t count,
		const pdo_duckdb_transaction_effect *effects)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	zend_long changed = 0;
	idx_t i;

	for (i = 0; i < count; i++) {
		duckdb_prepared_statement prepared = NULL;
		duckdb_result result;
		duckdb_statement_type type;

		if (duckdb_prepare_extracted_statement(H->conn, extracted, i, &prepared) != DuckDBSuccess) {
			pdo_duckdb_error(dbh, prepared ? duckdb_prepare_error(prepared) : "Unable to prepare DuckDB statement");
			duckdb_destroy_prepare(&prepared);
			return -1;
		}
		type = duckdb_prepared_statement_type(prepared);
		if (duckdb_execute_prepared(prepared, &result) != DuckDBSuccess) {
			pdo_duckdb_error(dbh, duckdb_result_error(&result));
			duckdb_destroy_result(&result);
			duckdb_destroy_prepare(&prepared);
			return -1;
		}
		changed = (zend_long)duckdb_rows_changed(&result);
		pdo_duckdb_sync_transaction_state(dbh, type, effects[i]);
		duckdb_destroy_result(&result);
		duckdb_destroy_prepare(&prepared);
	}
	return changed;
}

static zend_long duckdb_handle_doer(pdo_dbh_t *dbh, const zend_string *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;
	zend_long changed;
	unsigned int transaction_flags;
	pdo_duckdb_transaction_effect transaction_effect = PDO_DUCKDB_TRANSACTION_NONE;
	size_t statement_count;

	/* duckdb_query() takes a NUL-terminated const char*: an embedded NUL would
	 * silently truncate the statement. Reject it. */
	if (zend_str_has_nul_byte(sql)) {
		pdo_duckdb_error(dbh, "SQL statement contains a NUL byte");
		return -1;
	}

	transaction_flags = 0;
	if (pdo_duckdb_sql_may_have_multiple_statements(ZSTR_VAL(sql), ZSTR_LEN(sql))) {
		transaction_flags = pdo_duckdb_sql_transaction_flags(
			ZSTR_VAL(sql), ZSTR_LEN(sql), &transaction_effect, 1, &statement_count);
	} else {
		transaction_effect = pdo_duckdb_sql_first_transaction_effect(
			ZSTR_VAL(sql), ZSTR_LEN(sql));
	}

	if (!pdo_duckdb_enforce_sandbox(H)) {
		pdo_duckdb_error(dbh, "Unable to apply the open_basedir sandbox profile to DuckDB");
		return -1;
	}

	if ((transaction_flags & PDO_DUCKDB_SQL_MULTIPLE_STATEMENTS) &&
			(transaction_flags & (PDO_DUCKDB_SQL_DIRECT_TRANSACTION |
				PDO_DUCKDB_SQL_EXPLAIN_ANALYZE_TRANSACTION))) {
		duckdb_extracted_statements extracted = NULL;
		idx_t count = duckdb_extract_statements(H->conn, ZSTR_VAL(sql), &extracted);
		pdo_duckdb_transaction_effect *effects = ecalloc(
			statement_count, sizeof(pdo_duckdb_transaction_effect));
		size_t verified_statement_count;

		(void)pdo_duckdb_sql_transaction_flags(ZSTR_VAL(sql), ZSTR_LEN(sql),
			effects, statement_count, &verified_statement_count);

		if (count == 0) {
			pdo_duckdb_error(dbh, duckdb_extract_statements_error(extracted));
			efree(effects);
			duckdb_destroy_extracted(&extracted);
			return -1;
		}
		if (count > 1 && count == verified_statement_count) {
			changed = pdo_duckdb_exec_transaction_multi(dbh, extracted, count, effects);
			efree(effects);
			duckdb_destroy_extracted(&extracted);
			return changed;
		}
		efree(effects);
		duckdb_destroy_extracted(&extracted);
		if (count != verified_statement_count) {
			pdo_duckdb_error(dbh, "Unable to classify the DuckDB multi-statement transaction batch");
			return -1;
		}
	}

	if (duckdb_query(H->conn, ZSTR_VAL(sql), &result) != DuckDBSuccess) {
		pdo_duckdb_error(dbh, duckdb_result_error(&result));
		duckdb_destroy_result(&result);
		return -1;
	}

	changed = (zend_long)duckdb_rows_changed(&result);
	pdo_duckdb_sync_transaction_state(dbh, duckdb_result_statement_type(result),
		transaction_effect);
	duckdb_destroy_result(&result);
	return changed;
}

/* DuckDB uses standard SQL single-quote escaping (double the quote). */
static zend_string *duckdb_handle_quoter(pdo_dbh_t *dbh, const zend_string *unquoted, enum pdo_param_type paramtype)
{
	const char *src = ZSTR_VAL(unquoted);
	size_t srclen = ZSTR_LEN(unquoted);
	size_t i;
	smart_str buf = {0};

	if (UNEXPECTED(zend_str_has_nul_byte(unquoted))) {
		if (dbh->error_mode == PDO_ERRMODE_EXCEPTION) {
			zend_throw_exception_ex(php_pdo_get_exception(), 0,
				"DuckDB PDO::quote does not support null bytes");
		} else if (dbh->error_mode == PDO_ERRMODE_WARNING) {
			php_error_docref(NULL, E_WARNING,
				"DuckDB PDO::quote does not support null bytes");
		}
		return NULL;
	}

	smart_str_appendc(&buf, '\'');
	for (i = 0; i < srclen; i++) {
		if (src[i] == '\'') {
			smart_str_appendc(&buf, '\'');
		}
		smart_str_appendc(&buf, src[i]);
	}
	smart_str_appendc(&buf, '\'');
	smart_str_0(&buf);

	return buf.s;
}

/* Run a control statement (transaction verbs) that returns no rowset. */
static bool duckdb_simple_exec(pdo_dbh_t *dbh, const char *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;

	if (duckdb_query(H->conn, sql, &result) != DuckDBSuccess) {
		pdo_duckdb_error(dbh, duckdb_result_error(&result));
		duckdb_destroy_result(&result);
		return false;
	}
	duckdb_destroy_result(&result);
	return true;
}

static bool duckdb_handle_begin(pdo_dbh_t *dbh)
{
	return duckdb_simple_exec(dbh, "BEGIN TRANSACTION");
}

static bool duckdb_handle_commit(pdo_dbh_t *dbh)
{
	return duckdb_simple_exec(dbh, "COMMIT");
}

static bool duckdb_handle_rollback(pdo_dbh_t *dbh)
{
	return duckdb_simple_exec(dbh, "ROLLBACK");
}

static bool pdo_duckdb_set_attr(pdo_dbh_t *dbh, zend_long attr, zval *val)
{
	switch (attr) {
		case PDO_ATTR_AUTOCOMMIT:
			/* DuckDB is autocommit-by-default with no session-level toggle;
			 * explicit transactions go through begin/commit/rollback. Accept a
			 * request to keep autocommit on (the real behaviour), but reject
			 * turning it off rather than silently ignoring it — accepting false
			 * would mislead the caller into thinking statements are batched into
			 * a transaction when each still commits on its own. */
			if (zend_is_true(val)) {
				return true;
			}
			pdo_duckdb_error(dbh, "DuckDB does not support disabling autocommit; "
				"use beginTransaction() for explicit transactions");
			return false;

		case PDO_ATTR_PERSISTENT:
			/* PDO core manages persistence; accept so it can be passed as a
			 * constructor option without raising IM001. */
			return true;

		case PDO_DUCKDB_ATTR_UNBUFFERED:
			((pdo_duckdb_db_handle *)dbh->driver_data)->unbuffered = zend_is_true(val);
			return true;

		case PDO_DUCKDB_ATTR_CONFIG:
		{
			pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
			/* Consumed at open time by the handle factory (DuckDB config is
			 * open-time only). PDO core re-applies constructor driver-options
			 * through set_attribute after the factory; accept that one reapply,
			 * then reject runtime attempts instead of pretending to reconfigure
			 * an already-open database. */
			if (H->config_reapply_pending) {
				H->config_reapply_pending = false;
				return true;
			}
			pdo_duckdb_error(dbh, "PDO::DUCKDB_ATTR_CONFIG can only be supplied when opening a DuckDB connection");
			return false;
		}

		default:
			return false;
	}
}

static bool pdo_duckdb_query_ok(duckdb_connection conn, const char *sql)
{
	duckdb_result res;

	if (duckdb_query(conn, sql, &res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return false;
	}
	duckdb_destroy_result(&res);
	return true;
}

/* Disable DuckDB external access (read_csv/COPY/ATTACH/httpfs) on the live
 * connection. Clear allowlists before flipping enable_external_access: DuckDB
 * refuses allowed_directories/allowed_paths changes after external access is
 * disabled, but pre-existing allowlists remain effective. */
static bool pdo_duckdb_disable_external_access(pdo_duckdb_db_handle *H)
{
	if (!pdo_duckdb_query_ok(H->conn, "SET allowed_directories = []") ||
			!pdo_duckdb_query_ok(H->conn, "SET allowed_paths = []") ||
			!pdo_duckdb_query_ok(H->conn, "SET autoinstall_known_extensions=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET autoload_known_extensions=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET allow_community_extensions=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET allow_extensions_metadata_mismatch=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET allow_persistent_secrets=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET allow_unsigned_extensions=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET enable_external_file_cache=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET enable_http_metadata_cache=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET enable_external_access=false") ||
			!pdo_duckdb_query_ok(H->conn, "SET lock_configuration=true")) {
		return false;
	}
	H->external_access_disabled = true;
	return true;
}

/* If the open_basedir sandbox is required for this request but this handle was
 * opened without it (open_basedir unset at open time, then tightened — whether
 * across a persistent reuse or mid-request on a normal handle), disable external
 * access on the live connection before any SQL runs. One-way in DuckDB, so once
 * applied it stays; the flag short-circuits subsequent calls (one SET per
 * handle, lifetime). */
bool pdo_duckdb_enforce_sandbox(pdo_duckdb_db_handle *H)
{
	if ((PG(open_basedir) && *PG(open_basedir)) && !H->external_access_disabled) {
		return pdo_duckdb_disable_external_access(H);
	}
	return true;
}

/* Persistent handles skip handle_factory on reuse, so PDO calls check_liveness
 * on every reuse — escalate the sandbox there (rather than returning FAILURE,
 * which forces a rebuild whose discarded persistent dbh leaks in PDO core). */
static zend_result pdo_duckdb_check_liveness(pdo_dbh_t *dbh)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;

	H->unbuffered = false;
	return pdo_duckdb_enforce_sandbox(H)
		? SUCCESS : FAILURE;
}

static int pdo_duckdb_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *return_value)
{
	switch (attr) {
		case PDO_ATTR_CLIENT_VERSION:
		case PDO_ATTR_SERVER_VERSION:
			ZVAL_STRING(return_value, (char *)duckdb_library_version());
			break;

		case PDO_ATTR_DRIVER_NAME:
			ZVAL_STRINGL(return_value, "duckdb", sizeof("duckdb") - 1);
			break;

		case PDO_DUCKDB_ATTR_UNBUFFERED:
			ZVAL_BOOL(return_value, ((pdo_duckdb_db_handle *)dbh->driver_data)->unbuffered);
			break;

		default:
			return 0;
	}

	return 1;
}

/* Designated initializers: order-independent and only sets the slots we
 * implement. Fields absent on older PDO (e.g. scanner, added after 8.3) are
 * simply not named, so the table compiles cleanly on every supported PHP. */
static const struct pdo_dbh_methods duckdb_methods = {
	.closer = duckdb_handle_closer,
	.preparer = duckdb_handle_preparer,
	.doer = duckdb_handle_doer,
	.quoter = duckdb_handle_quoter,
	.begin = duckdb_handle_begin,
	.commit = duckdb_handle_commit,
	.rollback = duckdb_handle_rollback,
	.set_attribute = pdo_duckdb_set_attr,
	.fetch_err = pdo_duckdb_fetch_error_func,
	.get_attribute = pdo_duckdb_get_attribute,
	.check_liveness = pdo_duckdb_check_liveness,
	.get_driver_methods = pdo_duckdb_get_driver_methods,
	/* last_id: DuckDB has no implicit rowid; use sequences + currval(). */
	/* in_transaction: NULL -> PDO uses its internal transaction tracking. */
};

/* Resolve the DSN to a path DuckDB can open, enforcing open_basedir for plain
 * filesystem paths. Returns an emalloc'd string the caller must efree, or NULL
 * for in-memory (the empty / ":memory:" data source). On failure returns NULL
 * and sets *deny_reason to a message describing why (otherwise *deny_reason is
 * NULL); the two failure modes — unresolvable path vs open_basedir denial — get
 * distinct messages so the error isn't misattributed. */
static char *duckdb_make_path_safe(const char *data_source, const char **deny_reason)
{
	*deny_reason = NULL;

	if (!data_source || !*data_source || strcmp(data_source, ":memory:") == 0) {
		return NULL;  /* in-memory database */
	}

	char *fullpath = expand_filepath(data_source, NULL);
	if (!fullpath) {
		*deny_reason = "the path could not be resolved";
		return NULL;
	}

	if (php_check_open_basedir(fullpath)) {
		efree(fullpath);
		*deny_reason = "open_basedir prohibits it";
		return NULL;
	}

	return fullpath;
}

/* Set one DuckDB config option, throwing a clear error (and destroying the
 * config) on an invalid name/value so the caller can fail the open. */
static bool pdo_duckdb_set_one_config(duckdb_config *config, const char *key, const char *value)
{
	if (duckdb_set_config(*config, key, value) != DuckDBSuccess) {
		duckdb_destroy_config(config);
		*config = NULL;
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"Invalid DuckDB configuration option \"%s\"", key);
		return false;
	}
	return true;
}

static bool pdo_duckdb_driver_options_has_config(zval *driver_options)
{
	return driver_options && Z_TYPE_P(driver_options) == IS_ARRAY
		&& zend_hash_index_exists(Z_ARRVAL_P(driver_options), PDO_DUCKDB_ATTR_CONFIG);
}

static bool pdo_duckdb_config_key_matches(const char *key, size_t key_len, const char *name, size_t name_len)
{
	return key_len == name_len && zend_binary_strcasecmp(key, key_len, name, name_len) == 0;
}

#define PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, name) \
	pdo_duckdb_config_key_matches((key), (key_len), (name), sizeof(name) - 1)

static bool pdo_duckdb_sandbox_forbids_config_key(const char *key, size_t key_len)
{
	return PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allowed_directories") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allowed_paths") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "file_search_path") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "temp_directory") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "extension_directory") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "extension_directories") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "custom_extension_repository") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "autoinstall_extension_repository") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "autoinstall_known_extensions") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "autoload_known_extensions") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_community_extensions") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_extensions_metadata_mismatch") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_persistent_secrets") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_unredacted_secrets") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_unsigned_extensions") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "default_secret_storage") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "enable_external_file_cache") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "enable_http_metadata_cache") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "home_directory") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "http_logging_output") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "log_query_path") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "secret_directory");
}

#undef PDO_DUCKDB_CONFIG_KEY_MATCHES

static bool pdo_duckdb_reject_sandbox_config_key(duckdb_config *config, const char *key, size_t key_len)
{
	if (pdo_duckdb_sandbox_forbids_config_key(key, key_len)) {
		if (*config) {
			duckdb_destroy_config(config);
		}
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"DuckDB configuration option \"%.*s\" is not allowed when open_basedir is set",
			(int)key_len, key);
		return false;
	}
	return true;
}

static bool pdo_duckdb_apply_sandbox_config(duckdb_config *config)
{
	static const char *const sandbox_options[][2] = {
		{"autoinstall_known_extensions", "false"},
		{"autoload_known_extensions", "false"},
		{"allow_community_extensions", "false"},
		{"allow_extensions_metadata_mismatch", "false"},
		{"allow_persistent_secrets", "false"},
		{"allow_unsigned_extensions", "false"},
		{"enable_external_file_cache", "false"},
		{"enable_http_metadata_cache", "false"},
		{"enable_external_access", "false"},
		{"lock_configuration", "true"},
	};
	size_t i;

	for (i = 0; i < sizeof(sandbox_options) / sizeof(sandbox_options[0]); i++) {
		if (duckdb_set_config(*config, sandbox_options[i][0], sandbox_options[i][1]) != DuckDBSuccess) {
			duckdb_destroy_config(config);
			*config = NULL;
			zend_throw_exception_ex(php_pdo_get_exception(), 0,
				"Unable to apply the open_basedir sandbox (%s) to DuckDB",
				sandbox_options[i][0]);
			return false;
		}
	}
	return true;
}

static const char *pdo_duckdb_open_error_message(const char *open_error)
{
	if (open_error && strstr(open_error, "options were not recognized:")) {
		return open_error;
	}
	return "Unable to open DuckDB database";
}

static zend_string *pdo_duckdb_config_scalar_to_string(zval *value)
{
	ZVAL_DEREF(value);

	switch (Z_TYPE_P(value)) {
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
			return zval_get_string(value);
		default:
			zend_throw_exception_ex(php_pdo_get_exception(), 0,
				"PDO::DUCKDB_ATTR_CONFIG values must be scalar or null, %s given",
				zend_zval_type_name(value));
			return NULL;
	}
}

/* Build the DuckDB open-time config from user options and the open_basedir
 * sandbox. Sources, applied in this order so the sandbox always wins:
 *   1. DSN "key=value;..." pairs (everything after the first ';' in the DSN)
 *   2. the PDO::DUCKDB_ATTR_CONFIG array from driver_options (overrides DSN)
 *   3. if open_basedir is set: reject security/path-sensitive user options, then
 *      apply a locked sandbox profile LAST so a user attempt to re-enable external
 *      access can't defeat the sandbox.
 * *out_config is NULL when nothing needs setting. Returns false (with an
 * exception thrown) on a bad option, so the caller refuses to open. */
static bool pdo_duckdb_build_config(pdo_dbh_t *dbh, const char *dsn_opts,
		zval *driver_options, duckdb_config *out_config)
{
	duckdb_config config = NULL;
	bool sandbox = (PG(open_basedir) && *PG(open_basedir));

	*out_config = NULL;

#define DUCKDB_ENSURE_CONFIG() do { \
		if (!config) { \
			if (duckdb_create_config(&config) != DuckDBSuccess) { \
				zend_throw_exception_ex(php_pdo_get_exception(), 0, \
					"Unable to allocate DuckDB configuration"); \
				return false; \
			} \
		} \
	} while (0)

	/* 1. DSN key=value pairs */
	if (dsn_opts && *dsn_opts) {
		char *copy = estrdup(dsn_opts);
		char *save = NULL;
		char *pair = php_strtok_r(copy, ";", &save);

		while (pair) {
			char *eq = strchr(pair, '=');
			if (eq && eq != pair) {
				*eq = '\0';
				if (sandbox && !pdo_duckdb_reject_sandbox_config_key(&config, pair, strlen(pair))) {
					efree(copy);
					return false;
				}
				DUCKDB_ENSURE_CONFIG();
				if (!pdo_duckdb_set_one_config(&config, pair, eq + 1)) {
					efree(copy);
					return false;
				}
			} else if (*pair) {
				/* a non-empty segment without '=' is malformed; a valid option may
				 * already have allocated config, so destroy it before bailing.
				 * Do not echo the raw segment: DSN option tails can contain secrets. */
				if (config) {
					duckdb_destroy_config(&config);
				}
				zend_throw_exception_ex(php_pdo_get_exception(), 0,
					"Malformed DuckDB DSN option (expected key=value)");
				efree(copy);
				return false;
			}
			pair = php_strtok_r(NULL, ";", &save);
		}
		efree(copy);
	}

	/* 2. PDO::DUCKDB_ATTR_CONFIG => [ "key" => value, ... ] */
	if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
		zval *cfg = zend_hash_index_find(Z_ARRVAL_P(driver_options), PDO_DUCKDB_ATTR_CONFIG);
		if (cfg) {
			zend_string *key;
			zval *val;

			ZVAL_DEREF(cfg);
			if (Z_TYPE_P(cfg) != IS_ARRAY) {
				if (config) {
					duckdb_destroy_config(&config);
				}
				zend_throw_exception_ex(php_pdo_get_exception(), 0,
					"PDO::DUCKDB_ATTR_CONFIG must be an array of option => value");
				return false;
			}

			ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(cfg), key, val) {
				zend_string *sval;

				if (!key) {
					if (config) {
						duckdb_destroy_config(&config);
					}
					zend_throw_exception_ex(php_pdo_get_exception(), 0,
						"PDO::DUCKDB_ATTR_CONFIG keys must be option-name strings");
					return false;
				}
				/* duckdb_set_config() takes NUL-terminated strings; an embedded NUL
				 * in the option name or value would be silently truncated, applying
				 * a different (possibly security-relevant) option than requested. */
				if (zend_str_has_nul_byte(key)) {
					if (config) {
						duckdb_destroy_config(&config);
					}
					zend_throw_exception_ex(php_pdo_get_exception(), 0,
						"PDO::DUCKDB_ATTR_CONFIG option names and values must not contain a NUL byte");
					return false;
				}
				sval = pdo_duckdb_config_scalar_to_string(val);
				if (!sval) {
					if (config) {
						duckdb_destroy_config(&config);
					}
					return false;
				}
				if (zend_str_has_nul_byte(sval)) {
					zend_string_release(sval);
					if (config) {
						duckdb_destroy_config(&config);
					}
					zend_throw_exception_ex(php_pdo_get_exception(), 0,
						"PDO::DUCKDB_ATTR_CONFIG option names and values must not contain a NUL byte");
					return false;
				}
				if (sandbox && !pdo_duckdb_reject_sandbox_config_key(&config, ZSTR_VAL(key), ZSTR_LEN(key))) {
					zend_string_release(sval);
					return false;
				}
				DUCKDB_ENSURE_CONFIG();
				if (!pdo_duckdb_set_one_config(&config, ZSTR_VAL(key), ZSTR_VAL(sval))) {
					zend_string_release(sval);
					return false;
				}
				zend_string_release(sval);
			} ZEND_HASH_FOREACH_END();
		}
	}

	/* 3. open_basedir sandbox profile — applied last so it overrides any user
	 * setting that is safe to override. Security/path-sensitive settings were
	 * rejected above. This is a security boundary: fail closed if it can't be
	 * applied. */
	if (sandbox) {
		DUCKDB_ENSURE_CONFIG();
		if (!pdo_duckdb_apply_sandbox_config(&config)) {
			return false;
		}
	}

#undef DUCKDB_ENSURE_CONFIG

	*out_config = config;
	return true;
}

static int pdo_duckdb_handle_factory(pdo_dbh_t *dbh, zval *driver_options) /* {{{ */
{
	pdo_duckdb_db_handle *H;
	int ret = 0;
	char *path;
	char *path_dsn = NULL;
	const char *dsn_opts = NULL;
	const char *semi;
	const char *display_source;
	char *open_error = NULL;
	const char *deny_reason = NULL;
	duckdb_config config = NULL;
	duckdb_state open_state;

	H = pecalloc(1, sizeof(pdo_duckdb_db_handle), dbh->is_persistent);
	H->einfo.errcode = 0;
	H->einfo.errmsg = NULL;
	H->config_reapply_pending = pdo_duckdb_driver_options_has_config(driver_options);
	dbh->driver_data = H;

	if (dbh->is_persistent && H->config_reapply_pending) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"PDO::DUCKDB_ATTR_CONFIG cannot be used with persistent DuckDB connections");
		goto cleanup;
	}

	/* Split the DSN at the first ';': the path is the part before it, and any
	 * "key=value;..." tail is DuckDB config (handled by pdo_duckdb_build_config).
	 * ":memory:" and bare file paths contain no ';', so existing DSNs are
	 * unaffected. */
	semi = strchr(dbh->data_source, ';');
	if (semi) {
		path_dsn = estrndup(dbh->data_source, semi - dbh->data_source);
		dsn_opts = semi + 1;
	}

	display_source = path_dsn ? path_dsn : dbh->data_source;
	path = duckdb_make_path_safe(display_source, &deny_reason);
	if (deny_reason) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"Cannot open DuckDB database %s: %s", display_source, deny_reason);
		if (path_dsn) {
			efree(path_dsn);
		}
		goto cleanup;
	}
	if (path_dsn) {
		efree(path_dsn);
	}

	/* Build the open-time config from user DSN/attr options plus the open_basedir
	 * sandbox. open_basedir guards only the DB file path above; DuckDB SQL
	 * (read_csv, COPY, ATTACH, httpfs, ...) can otherwise reach the filesystem
	 * directly, so the sandbox profile disables external access, blocks extension
	 * autoload/install, and locks runtime configuration. A security boundary: fail
	 * closed — if the config can't be built, refuse to open rather than connect
	 * unsandboxed. */
	if (!pdo_duckdb_build_config(dbh, dsn_opts, driver_options, &config)) {
		if (path) {
			efree(path);
		}
		goto cleanup;  /* exception already thrown */
	}
	if (PG(open_basedir) && *PG(open_basedir)) {
		H->external_access_disabled = true;
	}

	/* Carry an opt-in unbuffered request through to statements on this handle. */
	if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
		zval *unbuf = zend_hash_index_find(Z_ARRVAL_P(driver_options), PDO_DUCKDB_ATTR_UNBUFFERED);
		if (unbuf) {
			ZVAL_DEREF(unbuf);
			H->unbuffered = zend_is_true(unbuf);
		}
	}

	open_state = duckdb_open_ext(path, &H->db, config, &open_error);
	if (config) {
		duckdb_destroy_config(&config);
	}
	if (open_state != DuckDBSuccess) {
		pdo_duckdb_error(dbh, pdo_duckdb_open_error_message(open_error));
		if (open_error) {
			duckdb_free(open_error);
		}
		if (path) {
			efree(path);
		}
		goto cleanup;
	}
	if (open_error) {
		duckdb_free(open_error);
		open_error = NULL;
	}
	if (path) {
		efree(path);
	}

	if (duckdb_connect(H->db, &H->conn) != DuckDBSuccess) {
		pdo_duckdb_error(dbh, "Unable to connect to DuckDB database");
		goto cleanup;
	}

	dbh->alloc_own_columns = 1;
	dbh->max_escaped_char_length = 2;

	ret = 1;

cleanup:
	dbh->methods = &duckdb_methods;

	return ret;
}
/* }}} */

/* {{{ Driver-specific helper methods.
 * Shared by the Pdo\Duckdb subclass (8.4+) and the base-PDO get_driver_methods
 * vehicle (8.1-8.3); the ZEND_METHOD wrappers for both classes delegate here.
 * The method tables live in the regenerated arginfo (included by
 * duckdb_appender.c). */

/* DuckDB's qualified table names append the query alias as " AS <alias>"
 * (e.g. "s.orders AS o"); the unaliased form is "s.orders". An unquoted
 * identifier cannot contain a space, so any top-level " AS " is unambiguously
 * the alias separator (a quoted identifier that contained it would be inside
 * double quotes). Strip it so the result is a usable schema.table name. */
static zend_string *pdo_duckdb_strip_table_alias(const char *s, size_t len)
{
	bool in_quote = false;
	size_t i, cut = len;

	for (i = 0; i + 4 <= len; i++) {
		if (s[i] == '"') {
			in_quote = !in_quote;
		} else if (!in_quote && s[i] == ' ' && s[i + 1] == 'A' && s[i + 2] == 'S' && s[i + 3] == ' ') {
			cut = i;
		}
	}
	return zend_string_init(s, cut, 0);
}

static void pdo_duckdb_table_names_impl(INTERNAL_FUNCTION_PARAMETERS)
{
	zend_string *query;
	bool qualified = false;
	pdo_dbh_t *dbh;
	pdo_duckdb_db_handle *H;
	duckdb_value list;
	idx_t n, i;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(query)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(qualified)
	ZEND_PARSE_PARAMETERS_END();

	dbh = Z_PDO_DBH_P(ZEND_THIS);
	PDO_CONSTRUCT_CHECK;
	H = (pdo_duckdb_db_handle *)dbh->driver_data;

	/* duckdb_get_table_names() takes a NUL-terminated const char*; an embedded
	 * NUL would silently truncate the query. Reject it. */
	if (zend_str_has_nul_byte(query)) {
		zend_value_error("PDO::duckdbTableNames(): query must not contain a NUL byte");
		RETURN_THROWS();
	}

	/* get_table_names parses and catalog-binds the query, so treat it as a SQL
	 * entry point: latch the open_basedir sandbox first (one-way, idempotent) so
	 * the invariant holds unconditionally rather than relying on get_table_names
	 * never touching the filesystem during bind. */
	if (!pdo_duckdb_enforce_sandbox(H)) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"PDO::duckdbTableNames(): unable to apply the open_basedir sandbox");
		RETURN_THROWS();
	}

	list = duckdb_get_table_names(H->conn, ZSTR_VAL(query), qualified);
	if (!list) {
		/* NULL means the query did not parse. get_table_names exposes no error
		 * detail; prepare() the query for the specific message. */
		zend_throw_exception_ex(php_pdo_get_exception(), 0,
			"PDO::duckdbTableNames(): could not parse the query");
		RETURN_THROWS();
	}

	array_init(return_value);
	n = duckdb_get_list_size(list);
	for (i = 0; i < n; i++) {
		duckdb_value e = duckdb_get_list_child(list, i);
		char *s = duckdb_get_varchar(e);
		if (s) {
			add_next_index_str(return_value,
				qualified ? pdo_duckdb_strip_table_alias(s, strlen(s))
				          : zend_string_init(s, strlen(s), 0));
			duckdb_free(s);
		}
		duckdb_destroy_value(&e);
	}
	duckdb_destroy_value(&list);
}

/* Render a duckdb_profiling_info node (and its subtree) into a PHP array shaped
 * ['metrics' => array<string,string>, 'children' => list]. Metrics arrive as a
 * MAP<VARCHAR,VARCHAR>, so every value is a string the caller casts as needed. */
static void pdo_duckdb_build_profile_node(duckdb_profiling_info info, zval *out)
{
	zval metrics, children;
	duckdb_value m;
	idx_t nchild, i;

	array_init(out);

	array_init(&metrics);
	m = duckdb_profiling_info_get_metrics(info);
	if (m) {
		idx_t ms = duckdb_get_map_size(m);
		for (i = 0; i < ms; i++) {
			duckdb_value k = duckdb_get_map_key(m, i);
			duckdb_value v = duckdb_get_map_value(m, i);
			/* duckdb_get_varchar() aborts the process on a NULL value (it throws
			 * a C++ InternalException), so guard with is_null first — like
			 * pdo_duckdb_col_to_string. A NULL-valued metric becomes PHP null. */
			if (!duckdb_is_null_value(k)) {
				char *ks = duckdb_get_varchar(k);
				if (ks) {
					if (duckdb_is_null_value(v)) {
						add_assoc_null(&metrics, ks);
					} else {
						char *vs = duckdb_get_varchar(v);
						if (vs) {
							add_assoc_string(&metrics, ks, vs);
							duckdb_free(vs);
						}
					}
					duckdb_free(ks);
				}
			}
			duckdb_destroy_value(&k);
			duckdb_destroy_value(&v);
		}
		duckdb_destroy_value(&m);
	}
	add_assoc_zval(out, "metrics", &metrics);

	array_init(&children);
	nchild = duckdb_profiling_info_get_child_count(info);
	for (i = 0; i < nchild; i++) {
		zval child;
		pdo_duckdb_build_profile_node(duckdb_profiling_info_get_child(info, i), &child);
		add_next_index_zval(&children, &child);
	}
	add_assoc_zval(out, "children", &children);
}

static void pdo_duckdb_last_profile_impl(INTERNAL_FUNCTION_PARAMETERS)
{
	pdo_dbh_t *dbh;
	pdo_duckdb_db_handle *H;
	duckdb_profiling_info info;

	ZEND_PARSE_PARAMETERS_NONE();

	dbh = Z_PDO_DBH_P(ZEND_THIS);
	PDO_CONSTRUCT_CHECK;
	H = (pdo_duckdb_db_handle *)dbh->driver_data;

	/* Reads the profile of the last query already run on this connection; it
	 * executes nothing. NULL when profiling was never enabled. */
	info = duckdb_get_profiling_info(H->conn);
	if (!info) {
		RETURN_NULL();
	}
	pdo_duckdb_build_profile_node(info, return_value);
}

ZEND_METHOD(Pdo_Duckdb, duckdbTableNames)
{
	pdo_duckdb_table_names_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
ZEND_METHOD(PdoDuckDb_Ext, duckdbTableNames)
{
	pdo_duckdb_table_names_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
ZEND_METHOD(Pdo_Duckdb, duckdbLastProfile)
{
	pdo_duckdb_last_profile_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
ZEND_METHOD(PdoDuckDb_Ext, duckdbLastProfile)
{
	pdo_duckdb_last_profile_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

const pdo_driver_t pdo_duckdb_driver = {
	PDO_DRIVER_HEADER(duckdb),
	pdo_duckdb_handle_factory
};
