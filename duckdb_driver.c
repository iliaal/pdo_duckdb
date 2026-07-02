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
	pdo_duckdb_error_info *einfo = &H->einfo;

	einfo->file = file;
	einfo->line = line;

	if (msg && *msg) {
		einfo->errcode = 1;
		if (einfo->errmsg) {
			pefree(einfo->errmsg, dbh->is_persistent);
		}
		einfo->errmsg = pestrdup(msg, dbh->is_persistent);
		strncpy(*pdo_err, "HY000", sizeof(*pdo_err));
	} else {
		einfo->errcode = 0;
		strncpy(*pdo_err, PDO_ERR_NONE, sizeof(*pdo_err));
		return 0;
	}

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

static bool duckdb_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, pdo_stmt_t *stmt, zval *driver_options)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_stmt *S;
	zend_string *rewritten = NULL;
	int parse_ret;

	/* duckdb_prepare() takes a NUL-terminated const char*, so an embedded NUL
	 * would silently truncate the statement (e.g. "SELECT 1\0DROP ..." prepares
	 * as "SELECT 1"). Reject it rather than run a different query than intended. */
	if (zend_str_has_nul_byte(sql)) {
		pdo_duckdb_error(dbh, "SQL statement contains a NUL byte");
		return false;
	}

	if (!pdo_duckdb_enforce_sandbox(H)) {
		pdo_duckdb_error(dbh, "Unable to apply the open_basedir sandbox (enable_external_access) to DuckDB");
		return false;
	}

	S = ecalloc(1, sizeof(pdo_duckdb_stmt));
	S->H = H;
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

	if (rewritten) {
		zend_string_release(rewritten);
	}
	return true;
}

static zend_long duckdb_handle_doer(pdo_dbh_t *dbh, const zend_string *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;
	zend_long changed;

	/* duckdb_query() takes a NUL-terminated const char*: an embedded NUL would
	 * silently truncate the statement. Reject it. */
	if (zend_str_has_nul_byte(sql)) {
		pdo_duckdb_error(dbh, "SQL statement contains a NUL byte");
		return -1;
	}

	if (!pdo_duckdb_enforce_sandbox(H)) {
		pdo_duckdb_error(dbh, "Unable to apply the open_basedir sandbox (enable_external_access) to DuckDB");
		return -1;
	}

	if (duckdb_query(H->conn, ZSTR_VAL(sql), &result) != DuckDBSuccess) {
		pdo_duckdb_error(dbh, duckdb_result_error(&result));
		duckdb_destroy_result(&result);
		return -1;
	}

	changed = (zend_long)duckdb_rows_changed(&result);
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
			/* Consumed at open time by the handle factory (DuckDB config is
			 * open-time only). PDO core re-applies every constructor driver-option
			 * through set_attribute after the factory, so accept it here as a
			 * no-op rather than failing the connection. A later runtime
			 * setAttribute() likewise can't reconfigure an open database. */
			return true;

		default:
			return false;
	}
}

/* Disable DuckDB external access (read_csv/COPY/ATTACH/httpfs) on the live
 * connection. This is one-way in DuckDB — it cannot be re-enabled while the
 * database is running — which is exactly the fail-safe direction we want. */
static bool pdo_duckdb_disable_external_access(pdo_duckdb_db_handle *H)
{
	duckdb_result res;

	if (duckdb_query(H->conn, "SET enable_external_access=false", &res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return false;
	}
	duckdb_destroy_result(&res);
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
	return pdo_duckdb_enforce_sandbox((pdo_duckdb_db_handle *)dbh->driver_data)
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

/* Build the DuckDB open-time config from user options and the open_basedir
 * sandbox. Sources, applied in this order so the sandbox always wins:
 *   1. DSN "key=value;..." pairs (everything after the first ';' in the DSN)
 *   2. the PDO::DUCKDB_ATTR_CONFIG array from driver_options (overrides DSN)
 *   3. if open_basedir is set: enable_external_access=false, applied LAST so a
 *      user attempt to re-enable external access can't defeat the sandbox.
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
				ZVAL_DEREF(val);
				sval = zval_get_string(val);
				/* duckdb_set_config() takes NUL-terminated strings; an embedded NUL
				 * in the option name or value would be silently truncated, applying
				 * a different (possibly security-relevant) option than requested. */
				if (zend_str_has_nul_byte(key) || zend_str_has_nul_byte(sval)) {
					zend_string_release(sval);
					if (config) {
						duckdb_destroy_config(&config);
					}
					zend_throw_exception_ex(php_pdo_get_exception(), 0,
						"PDO::DUCKDB_ATTR_CONFIG option names and values must not contain a NUL byte");
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

	/* 3. open_basedir sandbox — applied last so it overrides any user setting.
	 * This is a security boundary: fail closed if it can't be applied. */
	if (sandbox) {
		DUCKDB_ENSURE_CONFIG();
		if (duckdb_set_config(config, "enable_external_access", "false") != DuckDBSuccess) {
			duckdb_destroy_config(&config);
			zend_throw_exception_ex(php_pdo_get_exception(), 0,
				"Unable to apply the open_basedir sandbox (enable_external_access) to DuckDB");
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
	dbh->driver_data = H;

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
	 * directly, so the sandbox disables external access and is applied last. A
	 * security boundary: fail closed — if the config can't be built, refuse to
	 * open rather than connect unsandboxed. */
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
		pdo_duckdb_error(dbh, open_error ? open_error : "Unable to open DuckDB database");
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
