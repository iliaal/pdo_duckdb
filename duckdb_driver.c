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
#include "pdo_duckdb_bridge_filesystem.h"
#include "pdo_duckdb_bridge_logging.h"
#include "pdo_duckdb_bridge_query.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#ifdef PHP_WIN32
# include "win32/time.h"
# include "win32/ioutil.h"
#else
# include <unistd.h>
# include <sys/stat.h>
#endif
#if PHP_VERSION_ID >= 80300
# include "zend_hrtime.h"
# define pdo_duckdb_hrtime_ns() ((uint64_t)zend_hrtime())
#else
# include "ext/standard/hrtime.h"
# define pdo_duckdb_hrtime_ns() ((uint64_t)php_hrtime_current())
#endif

typedef struct _pdo_duckdb_instance {
	struct _pdo_duckdb_instance *next;
	pdo_duckdb_db_handle *handles;
	char *path;
	bool has_identity;
	uint64_t device;
	uint64_t file;
	bool external_access_disabled;
	bool sandbox_refused;
	char *sandbox_basedir;
	char sandbox_refusal[256];
#ifdef ZTS
	MUTEX_T mutex;
#endif
} pdo_duckdb_instance;

static duckdb_instance_cache pdo_duckdb_cache;
static pdo_duckdb_instance *pdo_duckdb_instances;
#ifdef ZTS
static MUTEX_T pdo_duckdb_registry_mutex;
#endif
ZEND_TLS pdo_duckdb_instance *pdo_duckdb_locked_instance;
ZEND_TLS unsigned int pdo_duckdb_lock_depth;

static void pdo_duckdb_registry_lock(void)
{
#ifdef ZTS
	tsrm_mutex_lock(pdo_duckdb_registry_mutex);
#endif
}

static void pdo_duckdb_registry_unlock(void)
{
#ifdef ZTS
	tsrm_mutex_unlock(pdo_duckdb_registry_mutex);
#endif
}

zend_result pdo_duckdb_instances_minit(void)
{
	pdo_duckdb_cache = duckdb_create_instance_cache();
	if (!pdo_duckdb_cache) {
		return FAILURE;
	}
#ifdef ZTS
	pdo_duckdb_registry_mutex = tsrm_mutex_alloc();
#endif
	return SUCCESS;
}

void pdo_duckdb_instances_mshutdown(void)
{
	/* Zend destroys persistent PDO resources before module shutdown. The cache
	 * holds weak references, so it cannot extend any database's lifetime. */
	duckdb_destroy_instance_cache(&pdo_duckdb_cache);
#ifdef ZTS
	tsrm_mutex_free(pdo_duckdb_registry_mutex);
#endif
}

void pdo_duckdb_instance_lock(pdo_duckdb_db_handle *H)
{
	if (!H->instance) {
		return;
	}
	if (pdo_duckdb_locked_instance == H->instance) {
		pdo_duckdb_lock_depth++;
		return;
	}
#if ZEND_DEBUG
	ZEND_ASSERT(pdo_duckdb_locked_instance == NULL);
#endif
#ifdef ZTS
	tsrm_mutex_lock(H->instance->mutex);
#endif
	pdo_duckdb_locked_instance = H->instance;
	pdo_duckdb_lock_depth = 1;
}

void pdo_duckdb_instance_unlock(pdo_duckdb_db_handle *H)
{
	if (!H->instance) {
		return;
	}
#if ZEND_DEBUG
	ZEND_ASSERT(pdo_duckdb_locked_instance == H->instance && pdo_duckdb_lock_depth > 0);
#endif
	if (--pdo_duckdb_lock_depth == 0) {
		pdo_duckdb_locked_instance = NULL;
#ifdef ZTS
		tsrm_mutex_unlock(H->instance->mutex);
#endif
	}
}

static bool pdo_duckdb_file_identity(const char *path, uint64_t *device, uint64_t *file)
{
#ifdef PHP_WIN32
	wchar_t *wide = php_win32_ioutil_any_to_w(path);
	HANDLE handle;
	BY_HANDLE_FILE_INFORMATION info;
	bool found;
	if (!wide) {
		return false;
	}
	handle = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	found = GetFileInformationByHandle(handle, &info) != 0;
	CloseHandle(handle);
	if (found) {
		*device = info.dwVolumeSerialNumber;
		*file = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
	}
	return found;
#else
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	*device = (uint64_t)st.st_dev;
	*file = (uint64_t)st.st_ino;
	return true;
#endif
}

static bool pdo_duckdb_same_path(const char *left, const char *right)
{
#if defined(PHP_WIN32) || defined(__APPLE__)
	return strcasecmp(left, right) == 0;
#else
	return strcmp(left, right) == 0;
#endif
}

static void pdo_duckdb_instance_free(pdo_duckdb_instance *instance)
{
#ifdef ZTS
	tsrm_mutex_free(instance->mutex);
#endif
	free(instance->path);
	free(instance->sandbox_basedir);
	free(instance);
}

static char *pdo_duckdb_open_error_copy(const char *message)
{
	size_t length = strlen(message) + 1;
	char *copy = duckdb_malloc(length);

	if (copy) {
		memcpy(copy, message, length);
	}
	return copy;
}

static duckdb_state pdo_duckdb_open_instance(pdo_duckdb_db_handle *H, const char *path,
		duckdb_config config, char **error)
{
	pdo_duckdb_instance *instance = NULL;
	uint64_t device = 0, file = 0;
	bool identified;
	bool created = false;
	duckdb_state state = DuckDBError;
	pdd_bridge_error bridge_error = {0};

	if (pdd_bridge_filesystem_install(config, &bridge_error) != PDD_BRIDGE_OK) {
		*error = pdo_duckdb_open_error_copy(bridge_error.data ? bridge_error.data : "Unable to install the database filesystem");
		pdd_bridge_error_free(&bridge_error);
		return DuckDBError;
	}

	pdo_duckdb_registry_lock();
	identified = path && pdo_duckdb_file_identity(path, &device, &file);
	if (path) {
		for (instance = pdo_duckdb_instances; instance; instance = instance->next) {
			if (!instance->path) {
				continue;
			}
			if (pdo_duckdb_same_path(path, instance->path)) {
				if (instance->has_identity && (!identified || instance->device != device || instance->file != file)) {
					*error = pdo_duckdb_open_error_copy(strcmp(path, instance->path) != 0
						? "DuckDB cannot safely open distinct database paths that differ only by case simultaneously"
						: "The database path changed while another connection still uses it");
					goto cleanup;
				}
				break;
			}
			if (identified && instance->has_identity && instance->device == device && instance->file == file) {
				*error = pdo_duckdb_open_error_copy("A hard-link alias of this database file is already open; use the original database path");
				goto cleanup;
			}
		}
	}
	if (!instance) {
		instance = calloc(1, sizeof(*instance));
		if (!instance || (path && !(instance->path = strdup(path)))) {
			free(instance);
			*error = pdo_duckdb_open_error_copy("Unable to allocate a shared DuckDB instance");
			goto cleanup;
		}
#ifdef ZTS
		instance->mutex = tsrm_mutex_alloc();
#endif
		created = true;
	}
	H->instance = instance;
	pdo_duckdb_instance_lock(H);
	state = duckdb_get_or_create_from_cache(pdo_duckdb_cache, path, &H->db, config, error);
	if (state == DuckDBSuccess && pdd_bridge_filesystem_refresh(H->db, &bridge_error) != PDD_BRIDGE_OK) {
		duckdb_close(&H->db);
		if (*error) {
			duckdb_free(*error);
		}
		*error = pdo_duckdb_open_error_copy(bridge_error.data ? bridge_error.data : "Unable to verify the database filesystem");
		pdd_bridge_error_free(&bridge_error);
		state = DuckDBError;
	}
	if (state == DuckDBSuccess && duckdb_connect(H->db, &H->conn) != DuckDBSuccess) {
		duckdb_close(&H->db);
		*error = pdo_duckdb_open_error_copy("Unable to connect to DuckDB database");
		state = DuckDBError;
	}
	if (state == DuckDBSuccess && created && path) {
		instance->has_identity = pdo_duckdb_file_identity(path, &instance->device, &instance->file);
		if (!instance->has_identity || (identified &&
				(instance->device != device || instance->file != file))) {
			duckdb_disconnect(&H->conn);
			duckdb_close(&H->db);
			*error = pdo_duckdb_open_error_copy("Unable to verify the database file identity safely");
			state = DuckDBError;
		}
	}
	if (state == DuckDBSuccess) {
		if (created) {
			instance->next = pdo_duckdb_instances;
			pdo_duckdb_instances = instance;
		}
		H->instance_next = instance->handles;
		instance->handles = H;
	}
	pdo_duckdb_instance_unlock(H);
	if (state != DuckDBSuccess) {
		H->instance = NULL;
		if (created) {
			pdo_duckdb_instance_free(instance);
		}
	}
cleanup:
	pdo_duckdb_registry_unlock();
	return state;
}

void pdo_duckdb_clear_einfo(pdo_duckdb_error_info *einfo, bool persistent)
{
	if (einfo->errmsg) {
		pefree(einfo->errmsg, persistent);
		einfo->errmsg = NULL;
	}
	einfo->errcode = 0;
	einfo->file = NULL;
	einfo->line = 0;
}

static const char *pdo_duckdb_sqlstate_for_code(unsigned int code)
{
	switch (code) {
		case PDO_DUCKDB_ERRCODE_CONNECT:
			return "08000";
		case PDO_DUCKDB_ERRCODE_SYNTAX:
			return "42000";
		default:
			return "HY000";
	}
}

int _pdo_duckdb_error_with_code(pdo_dbh_t *dbh, pdo_stmt_t *stmt, unsigned int code, const char *msg, const char *file, int line) /* {{{ */
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_error_type *pdo_err = stmt ? &stmt->error_code : &dbh->error_code;
	pdo_duckdb_error_info *einfo;
	bool persistent = dbh->is_persistent;
	const char *errmsg = (msg && *msg) ? msg : "DuckDB operation failed";
	const char *sqlstate = pdo_duckdb_sqlstate_for_code(code);

	if (stmt && stmt->driver_data) {
		pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
		einfo = &S->einfo;
		persistent = false;
	} else {
		einfo = &H->einfo;
	}

	einfo->file = file;
	einfo->line = line;

	einfo->errcode = (int)code;
	if (einfo->errmsg) {
		pefree(einfo->errmsg, persistent);
	}
	einfo->errmsg = pestrdup(errmsg, persistent);
	strncpy(*pdo_err, sqlstate, sizeof(*pdo_err));

	if (!dbh->methods) {
		pdo_throw_exception(einfo->errcode, einfo->errmsg, pdo_err);
	}

	return einfo->errcode;
}
/* }}} */

int _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *msg, const char *file, int line) /* {{{ */
{
	return _pdo_duckdb_error_with_code(dbh, stmt, PDO_DUCKDB_ERRCODE_GENERAL, msg, file, line);
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
		pdo_duckdb_instance *instance = H->instance;
		pdo_duckdb_db_handle **member;
		pdo_duckdb_appenders_release(H);
		pdo_duckdb_registry_lock();
		pdo_duckdb_native_cleanup_enter(H);
		if (H->conn) {
			pdd_bridge_error error = {0};
			bool remote = false;
			/* A query that failed while DuckDB was starting it (e.g. a file log
			 * sink that cannot be opened) leaves a stale active query, which
			 * disconnect would end inside the ClientContext destructor; a second
			 * sink failure there calls std::terminate. An empty query ends any
			 * active query first, under duckdb_query's catch-all, and parses
			 * nothing. */
			if (pdd_bridge_connection_is_remote(H->conn, &remote, &error) == PDD_BRIDGE_OK && !remote) {
				duckdb_query(H->conn, "", NULL);
			}
			pdd_bridge_error_free(&error);
			duckdb_disconnect(&H->conn);
			H->conn = NULL;
		}
		if (H->db) {
			duckdb_close(&H->db);
			H->db = NULL;
		}
		if (instance) {
			for (member = &instance->handles; *member; member = &(*member)->instance_next) {
				if (*member == H) {
					*member = H->instance_next;
					break;
				}
			}
		}
		pdo_duckdb_instance_unlock(H);
		H->instance = NULL;
		if (instance && !instance->handles) {
			pdo_duckdb_instance **entry;
			for (entry = &pdo_duckdb_instances; *entry; entry = &(*entry)->next) {
				if (*entry == instance) {
					*entry = instance->next;
					break;
				}
			}
			pdo_duckdb_instance_free(instance);
		}
		pdo_duckdb_registry_unlock();
		pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
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

/* Snapshot before releasing the instance lock. Only the supplied native owners
 * need cleanup if PHP cannot allocate the error string. */
static zend_string *pdo_duckdb_native_error_copy_locked(pdo_duckdb_db_handle *H,
		const char *message, duckdb_prepared_statement *prepared,
		duckdb_result *result, duckdb_extracted_statements *extracted)
{
	zend_string *error = NULL;

	zend_try {
		const char *text = message && *message ? message : "DuckDB operation failed";

		error = zend_string_init(text, strlen(text), 0);
	} zend_catch {
		if (result) {
			duckdb_destroy_result(result);
		}
		if (prepared) {
			duckdb_destroy_prepare(prepared);
		}
		if (extracted) {
			duckdb_destroy_extracted(extracted);
		}
		pdo_duckdb_instance_unlock(H);
		zend_bailout();
	} zend_end_try();
	return error;
}

static bool duckdb_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, pdo_stmt_t *stmt, zval *driver_options)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_stmt *S;
	zend_string *volatile rewritten = NULL, *volatile probe = NULL, *volatile error = NULL;
	pdd_bridge_error bridge_error = {0};
	pdd_bridge_sql_info info;
	bool remote = false, attached = false, legacy_parser = true;
	pdd_bridge_status status;

	if (PDO_CURSOR_FWDONLY != pdo_attr_lval(driver_options, PDO_ATTR_CURSOR, PDO_CURSOR_FWDONLY)) {
		pdo_duckdb_error(dbh, "DuckDB PDO driver only supports forward-only cursors");
		return false;
	}
	if (zend_str_has_nul_byte(sql)) {
		pdo_duckdb_error(dbh, "SQL statement contains a NUL byte");
		return false;
	}

	S = ecalloc(1, sizeof(pdo_duckdb_stmt));
	S->H = H;
	S->native_structures = pdo_attr_lval(driver_options, PDO_DUCKDB_ATTR_NATIVE_STRUCTURES,
		H->native_structures) != 0;
	S->transaction_effect = pdo_duckdb_sql_first_transaction_effect(ZSTR_VAL(sql), ZSTR_LEN(sql));
	stmt->driver_data = S;
	stmt->methods = &duckdb_stmt_methods;
	stmt->supports_placeholders = PDO_PLACEHOLDER_NAMED;
	stmt->named_rewrite_template = "$%d";
	if (!pdo_duckdb_native_enter(H)) {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
		return false;
	}
	status = pdd_bridge_connection_uses_legacy_parser(H->conn, &legacy_parser, &bridge_error);
	if (status != PDD_BRIDGE_OK) {
		goto bridge_failure;
	}
	zend_try {
		zend_string *plan_error = NULL;
		S->sql_plan = pdo_duckdb_sql_plan_create(sql, legacy_parser, &plan_error);
		error = plan_error;
		if (S->sql_plan) {
			bool selected_legacy_parser = legacy_parser;
			probe = pdo_duckdb_sql_parse_probe(S->sql_plan);
			if (legacy_parser) {
				status = pdd_bridge_query_uses_legacy_parser(H->conn, ZSTR_VAL(probe), ZSTR_LEN(probe),
					&selected_legacy_parser, &bridge_error);
			}
			if (status == PDD_BRIDGE_OK && selected_legacy_parser != legacy_parser) {
				zend_string_release(probe);
				probe = NULL;
				pdo_duckdb_sql_plan_destroy(S->sql_plan);
				S->sql_plan = NULL;
				S->sql_plan = pdo_duckdb_sql_plan_create(sql, selected_legacy_parser, &plan_error);
				error = plan_error;
				if (S->sql_plan) {
					probe = pdo_duckdb_sql_parse_probe(S->sql_plan);
				}
			}
			if (status == PDD_BRIDGE_OK && S->sql_plan) {
				S->expanded_binding_count = S->sql_plan->binding_count;
				if (S->expanded_binding_count) {
					S->expanded_bindings = ecalloc(S->expanded_binding_count, sizeof(*S->expanded_bindings));
				}
				rewritten = pdo_duckdb_sql_native(S->sql_plan);
			}
		}
	} zend_catch {
		pdo_duckdb_instance_unlock(H);
		if (probe) {
			zend_string_release(probe);
		}
		if (rewritten) {
			zend_string_release(rewritten);
		}
		if (error) {
			zend_string_release(error);
		}
		zend_bailout();
	} zend_end_try();
	if (!S->sql_plan) {
		pdo_duckdb_instance_unlock(H);
		pdo_duckdb_error(dbh, ZSTR_VAL(error));
		memcpy(dbh->error_code, "HY093", sizeof("HY093"));
		zend_string_release(error);
		return false;
	}
	if (status != PDD_BRIDGE_OK) {
		goto bridge_failure;
	}
	status = pdd_bridge_connection_is_remote(H->conn, &remote, &bridge_error);
	if (status == PDD_BRIDGE_OK) {
		status = pdd_bridge_connection_has_remote_catalogs(H->conn, &attached, &bridge_error);
	}
	S->deferred_query = remote || attached ||
		(S->sql_plan->is_attach && S->sql_plan->parameter_count != 0);
#ifdef HAVE_DUCKDB_V2
	S->deferred_query = S->deferred_query ||
		(S->sql_plan->has_named_dollar_parameters && S->sql_plan->parameter_count != 0);
#endif
	if (status == PDD_BRIDGE_OK && S->deferred_query) {
		status = pdd_bridge_query_classify(H->conn, ZSTR_VAL(probe), ZSTR_LEN(probe),
			&info, &bridge_error);
	}
	zend_string_release(probe);
	probe = NULL;
	if (status != PDD_BRIDGE_OK) {
		goto bridge_failure;
	}
	duckdb_state prepared_status = S->deferred_query ? DuckDBSuccess :
		duckdb_prepare(H->conn, ZSTR_VAL(rewritten), &S->prepared);
	zend_string_release(rewritten);
	rewritten = NULL;
	if (prepared_status != DuckDBSuccess) {
		error = pdo_duckdb_native_error_copy_locked(H,
			S->prepared ? duckdb_prepare_error(S->prepared) : "Unable to prepare DuckDB statement",
			&S->prepared, NULL, NULL);
		if (S->prepared) {
			duckdb_destroy_prepare(&S->prepared);
		}
		pdo_duckdb_instance_unlock(H);
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SYNTAX, ZSTR_VAL(error));
		zend_string_release(error);
		return false;
	}
	pdo_duckdb_instance_unlock(H);
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
	return true;

bridge_failure:
	pdo_duckdb_instance_unlock(H);
	if (probe) {
		zend_string_release(probe);
	}
	if (rewritten) {
		zend_string_release(rewritten);
	}
	zend_try {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SYNTAX,
			bridge_error.data ? bridge_error.data : "Unable to parse DuckDB statement");
	} zend_catch {
		pdd_bridge_error_free(&bridge_error);
		zend_bailout();
	} zend_end_try();
	pdd_bridge_error_free(&bridge_error);
	return false;
}

void pdo_duckdb_apply_transaction_effect(pdo_dbh_t *dbh,
		pdo_duckdb_transaction_effect effect)
{
	/* Only explicit OPEN/CLOSE effects move in_txn. Never toggle on bare
	 * TRANSACTION type: a missed keyword classification would flip the flag
	 * wrong and break free-time rollback / persistent cleanup. */
	if (effect == PDO_DUCKDB_TRANSACTION_OPEN) {
		dbh->in_txn = true;
		return;
	}
	if (effect == PDO_DUCKDB_TRANSACTION_CLOSE) {
		dbh->in_txn = false;
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

		/* Match DuckDB's libpg_query dolq_start/dolq_cont classes. The
		 * scanner works on bytes, so every UTF-8 continuation/lead byte in
		 * a valid non-ASCII tag is accepted just as DuckDB accepts it. Digits
		 * are valid after the first tag character (but not as its first). */
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				c == '_' || c >= 0x80 ||
				(tag_end > start + 1 && c >= '0' && c <= '9'))) {
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
		duckdb_result result = {0};

		if (!pdo_duckdb_native_enter(H)) {
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
			return -1;
		}
		if (duckdb_prepare_extracted_statement(H->conn, extracted, i, &prepared) != DuckDBSuccess) {
			zend_string *error = pdo_duckdb_native_error_copy_locked(H,
				prepared ? duckdb_prepare_error(prepared) : "Unable to prepare DuckDB statement",
				&prepared, &result, &extracted);
			duckdb_destroy_prepare(&prepared);
			pdo_duckdb_instance_unlock(H);
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SYNTAX, ZSTR_VAL(error));
			zend_string_release(error);
			return -1;
		}
		if (duckdb_execute_prepared(prepared, &result) != DuckDBSuccess) {
			zend_string *error = pdo_duckdb_native_error_copy_locked(H,
				duckdb_result_error(&result), &prepared, &result, &extracted);
			duckdb_destroy_result(&result);
			duckdb_destroy_prepare(&prepared);
			pdo_duckdb_instance_unlock(H);
			pdo_duckdb_error(dbh, ZSTR_VAL(error));
			zend_string_release(error);
			return -1;
		}
		changed = (zend_long)duckdb_rows_changed(&result);
		duckdb_destroy_result(&result);
		duckdb_destroy_prepare(&prepared);
		pdo_duckdb_instance_unlock(H);
		pdo_duckdb_apply_transaction_effect(dbh, effects[i]);
	}
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
	return changed;
}

#ifdef HAVE_DUCKDB_V2
static zend_long pdo_duckdb_exec_bridge_error(pdo_dbh_t *dbh, pdd_bridge_error *error, bool syntax)
{
	zend_try {
		pdo_duckdb_error_code(dbh, syntax ? PDO_DUCKDB_ERRCODE_SYNTAX : PDO_DUCKDB_ERRCODE_GENERAL,
			error->data ? error->data : "Unable to execute DuckDB statement");
	} zend_catch {
		pdd_bridge_error_free(error);
		zend_bailout();
	} zend_end_try();
	pdd_bridge_error_free(error);
	return -1;
}

/* The caller holds the instance lock. The bridge owns all native result cleanup. */
static zend_long pdo_duckdb_exec_bridge_locked(pdo_dbh_t *dbh, const zend_string *sql,
		bool force_last_count)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdd_bridge_exec_info info = {0};
	pdd_bridge_error error = {0};
	pdd_bridge_status status = pdd_bridge_exec_batch(H->conn, ZSTR_VAL(sql), ZSTR_LEN(sql),
		force_last_count, &info, &error);

	pdo_duckdb_instance_unlock(H);
	if (info.last_transaction_effect == PDD_BRIDGE_TRANSACTION_BEGIN) {
		pdo_duckdb_apply_transaction_effect(dbh, PDO_DUCKDB_TRANSACTION_OPEN);
	} else if (info.last_transaction_effect == PDD_BRIDGE_TRANSACTION_END) {
		pdo_duckdb_apply_transaction_effect(dbh, PDO_DUCKDB_TRANSACTION_CLOSE);
	}
	if (status != PDD_BRIDGE_OK) {
		return pdo_duckdb_exec_bridge_error(dbh, &error,
			force_last_count && (info.parse_failed || info.prepare_failed));
	}
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
	return (zend_long)info.rows_changed;
}
#endif

static zend_long duckdb_handle_doer(pdo_dbh_t *dbh, const zend_string *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result = {0};
	zend_long changed;
	unsigned int transaction_flags;
	pdo_duckdb_transaction_effect transaction_effect = PDO_DUCKDB_TRANSACTION_NONE;
	size_t statement_count;

	/* duckdb_query() would truncate at an embedded NUL. */
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

#ifdef HAVE_DUCKDB_V2
	{
		bool remote = false, attached = false;
		pdd_bridge_error error = {0};
		pdd_bridge_status status;

		if (!pdo_duckdb_native_enter(H)) {
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
			return -1;
		}
		status = pdd_bridge_connection_is_remote(H->conn, &remote, &error);
		if (status == PDD_BRIDGE_OK) {
			status = pdd_bridge_connection_has_remote_catalogs(H->conn, &attached, &error);
		}
		if (status != PDD_BRIDGE_OK) {
			pdo_duckdb_instance_unlock(H);
			return pdo_duckdb_exec_bridge_error(dbh, &error, false);
		}
		if (remote || attached || (transaction_flags & PDO_DUCKDB_SQL_MULTIPLE_STATEMENTS)) {
			bool force_last_count = (transaction_flags & (PDO_DUCKDB_SQL_DIRECT_TRANSACTION |
				PDO_DUCKDB_SQL_EXPLAIN_ANALYZE_TRANSACTION)) != 0;
			return pdo_duckdb_exec_bridge_locked(dbh, sql, force_last_count);
		}
		pdo_duckdb_instance_unlock(H);
	}
#endif

	if ((transaction_flags & PDO_DUCKDB_SQL_MULTIPLE_STATEMENTS) &&
			(transaction_flags & (PDO_DUCKDB_SQL_DIRECT_TRANSACTION |
				PDO_DUCKDB_SQL_EXPLAIN_ANALYZE_TRANSACTION))) {
		duckdb_extracted_statements extracted = NULL;
		idx_t count;
		pdo_duckdb_transaction_effect *effects = ecalloc(
			statement_count, sizeof(pdo_duckdb_transaction_effect));
		size_t verified_statement_count;

		(void)pdo_duckdb_sql_transaction_flags(ZSTR_VAL(sql), ZSTR_LEN(sql),
			effects, statement_count, &verified_statement_count);

		if (!pdo_duckdb_native_enter(H)) {
			efree(effects);
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
			return -1;
		}
		count = duckdb_extract_statements(H->conn, ZSTR_VAL(sql), &extracted);
		if (count == 0) {
			zend_string *error = pdo_duckdb_native_error_copy_locked(H,
				duckdb_extract_statements_error(extracted), NULL, NULL, &extracted);
			duckdb_destroy_extracted(&extracted);
			pdo_duckdb_instance_unlock(H);
			efree(effects);
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SYNTAX, ZSTR_VAL(error));
			zend_string_release(error);
			return -1;
		}
		pdo_duckdb_instance_unlock(H);
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

	if (!pdo_duckdb_native_enter(H)) {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
		return -1;
	}
	if (duckdb_query(H->conn, ZSTR_VAL(sql), &result) != DuckDBSuccess) {
		zend_string *error = pdo_duckdb_native_error_copy_locked(H,
			duckdb_result_error(&result), NULL, &result, NULL);
		duckdb_destroy_result(&result);
		pdo_duckdb_instance_unlock(H);
		pdo_duckdb_error(dbh, ZSTR_VAL(error));
		zend_string_release(error);
		return -1;
	}

	changed = (zend_long)duckdb_rows_changed(&result);
	duckdb_destroy_result(&result);
	pdo_duckdb_instance_unlock(H);
	pdo_duckdb_apply_transaction_effect(dbh, transaction_effect);
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
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
		/* Record error only; PDO core applies ERRMODE when quoter returns NULL. */
		pdo_duckdb_error(dbh, "DuckDB PDO::quote does not support null bytes");
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

	{
		pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
		pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
	}
	return buf.s;
}

static bool duckdb_simple_exec(pdo_dbh_t *dbh, const char *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result = {0};

	if (!pdo_duckdb_native_enter(H)) {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
		return false;
	}

	if (duckdb_query(H->conn, sql, &result) != DuckDBSuccess) {
		zend_string *error = pdo_duckdb_native_error_copy_locked(H,
			duckdb_result_error(&result), NULL, &result, NULL);
		duckdb_destroy_result(&result);
		pdo_duckdb_instance_unlock(H);
		pdo_duckdb_error(dbh, ZSTR_VAL(error));
		zend_string_release(error);
		return false;
	}
	duckdb_destroy_result(&result);
	pdo_duckdb_instance_unlock(H);
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
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
	pdo_duckdb_appenders_discard_for_rollback((pdo_duckdb_db_handle *)dbh->driver_data);
	return duckdb_simple_exec(dbh, "ROLLBACK");
}

static bool pdo_duckdb_zval_is_true(zval *val)
{
	ZVAL_DEREF(val);
	switch (Z_TYPE_P(val)) {
		case IS_TRUE:
			return true;
		case IS_FALSE:
		case IS_NULL:
			return false;
		case IS_LONG:
			return Z_LVAL_P(val) != 0;
		case IS_DOUBLE:
			return Z_DVAL_P(val) != 0.0;
		case IS_STRING:
			return Z_STRLEN_P(val) != 0 && !zend_string_equals_literal(Z_STR_P(val), "0");
		default:
			return zend_is_true(val);
	}
}

static bool pdo_duckdb_set_attr(pdo_dbh_t *dbh, zend_long attr, zval *val)
{
	switch (attr) {
		case PDO_ATTR_AUTOCOMMIT:
			/* DuckDB has no session-level autocommit toggle. */
			if (pdo_duckdb_zval_is_true(val)) {
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
			((pdo_duckdb_db_handle *)dbh->driver_data)->unbuffered = pdo_duckdb_zval_is_true(val);
			return true;
		case PDO_DUCKDB_ATTR_NATIVE_STRUCTURES:
			((pdo_duckdb_db_handle *)dbh->driver_data)->native_structures = pdo_duckdb_zval_is_true(val);
			return !EG(exception);

		case PDO_DUCKDB_ATTR_CONFIG:
		{
			pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
			/* PDO reapplies constructor options once; DuckDB config is open-time only. */
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
	duckdb_result res = {0};

	if (duckdb_query(conn, sql, &res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return false;
	}
	duckdb_destroy_result(&res);
	return true;
}

#define PDO_DUCKDB_HELPER_MAX_COLUMNS 2

/* cells[i] is a NUL-terminated copy of column i, or NULL for SQL NULL. Return
 * false to stop the walk. */
typedef bool (*pdo_duckdb_varchar_row_cb)(void *ctx, char **cells);

/* Walk a materialized result of ncols VARCHAR columns through the data-chunk
 * API; the duckdb_value_* row API misreads and crashes on DuckDB 2.0 results
 * with a NULL-typed column. Returns false, after *rows_out rows, on a column
 * shape mismatch, a cell holding a NUL byte (the callers need C strings; sets
 * *nul_cell), a fetch error, or a callback stop. */
static bool pdo_duckdb_each_varchar_row(duckdb_result *res, idx_t ncols,
		pdo_duckdb_varchar_row_cb cb, void *ctx, idx_t *rows_out, bool *nul_cell)
{
	duckdb_data_chunk chunk;
	idx_t col;
	bool ok = true;

	*rows_out = 0;
	*nul_cell = false;
	if (ncols == 0 || ncols > PDO_DUCKDB_HELPER_MAX_COLUMNS || duckdb_column_count(res) != ncols) {
		return false;
	}
	for (col = 0; col < ncols; col++) {
		if (duckdb_column_type(res, col) != DUCKDB_TYPE_VARCHAR) {
			return false;
		}
	}

	while (ok && (chunk = duckdb_fetch_chunk(*res)) != NULL) {
		idx_t size = duckdb_data_chunk_get_size(chunk);
		duckdb_string_t *data[PDO_DUCKDB_HELPER_MAX_COLUMNS];
		uint64_t *validity[PDO_DUCKDB_HELPER_MAX_COLUMNS];
		idx_t row;

		for (col = 0; col < ncols; col++) {
			duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, col);

			data[col] = (duckdb_string_t *)duckdb_vector_get_data(vec);
			validity[col] = duckdb_vector_get_validity(vec);
		}
		for (row = 0; ok && row < size; row++) {
			char *cells[PDO_DUCKDB_HELPER_MAX_COLUMNS] = {NULL};

			for (col = 0; col < ncols; col++) {
				const char *p;
				size_t len;

				if (validity[col] && !duckdb_validity_row_is_valid(validity[col], row)) {
					continue;
				}
				p = duckdb_string_t_data(&data[col][row]);
				len = (size_t)duckdb_string_t_length(data[col][row]);
				if (memchr(p, '\0', len)) {
					*nul_cell = true;
					ok = false;
					break;
				}
				cells[col] = estrndup(p, len);
			}
			if (ok) {
				(*rows_out)++;
				ok = cb(ctx, cells);
			}
			for (col = 0; col < ncols; col++) {
				if (cells[col]) {
					efree(cells[col]);
				}
			}
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	return ok && duckdb_result_error(res) == NULL;
}

static bool pdo_duckdb_result_has_rows(duckdb_result *res)
{
	duckdb_data_chunk chunk;

	while ((chunk = duckdb_fetch_chunk(*res)) != NULL) {
		idx_t size = duckdb_data_chunk_get_size(chunk);

		duckdb_destroy_data_chunk(&chunk);
		if (size > 0) {
			return true;
		}
	}
	return false;
}

static void pdo_duckdb_append_quoted(smart_str *sql, const char *s, char quote)
{
	smart_str_appendc(sql, quote);
	for (; *s; s++) {
		if (*s == quote) {
			smart_str_appendc(sql, quote);
		}
		smart_str_appendc(sql, *s);
	}
	smart_str_appendc(sql, quote);
}

static void pdo_duckdb_refuse_sandbox(pdo_duckdb_db_handle *H, const char *reason);

static bool pdo_duckdb_detach_row(void *ctx, char **cells)
{
	duckdb_connection conn = (duckdb_connection)ctx;
	const char *name = cells[0], *path = cells[1];
	smart_str sql = {0};
	bool ok;

	if (!name || !path || !*path) {
		return true;
	}
	/* Quiet check: denied attachments are detached, not user-facing warnings. */
	if (php_check_open_basedir_ex(path, 0) == 0) {
		return true;
	}

	smart_str_appends(&sql, "DETACH ");
	pdo_duckdb_append_quoted(&sql, name, '"');
	smart_str_0(&sql);
	ok = pdo_duckdb_query_ok(conn, ZSTR_VAL(sql.s));
	smart_str_free(&sql);
	return ok;
}

/* DETACH user-attached databases whose paths fall outside open_basedir.
 * OnSet re-adds every attached path to allowed_paths when external access is
 * disabled; those entries cannot be cleared afterward. */
static bool pdo_duckdb_detach_out_of_basedir(pdo_duckdb_db_handle *H, duckdb_connection conn)
{
	duckdb_result res = {0};
	idx_t rows;
	bool ok, nul_cell;

	if (duckdb_query(conn,
			"SELECT database_name, path FROM duckdb_databases() "
			"WHERE NOT internal AND path IS NOT NULL AND path <> ''",
			&res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return false;
	}

	ok = pdo_duckdb_each_varchar_row(&res, 2, pdo_duckdb_detach_row, conn, &rows, &nul_cell);
	duckdb_destroy_result(&res);
	if (nul_cell) {
		pdo_duckdb_refuse_sandbox(H, "an attached database name or path contains a NUL byte");
	}
	return ok;
}

/* Empty temp before the flip: a non-empty temp_directory is re-allowlisted by
 * DuckDB OnSet and cannot be cleared afterward (do not seed basedir root). */
static const char *const pdo_duckdb_profile_pre_detach[] = {
	/* A file log sink keeps writing through its open file after external
	 * access is off, and CALL enable_logging() re-arms it even after the lock;
	 * switching storage closes it. */
	"SET logging_storage='memory'",
	"SET allowed_configs = []",
	"SET temp_directory=''",
};

static const char *const pdo_duckdb_profile_post_detach[] = {
	"SET allowed_directories = []",
	"SET allowed_paths = []",
	"SET autoinstall_known_extensions=false",
	"SET autoload_known_extensions=false",
	"SET allow_community_extensions=false",
	"SET allow_extensions_metadata_mismatch=false",
	"SET allow_persistent_secrets=false",
	"SET allow_unredacted_secrets=false",
	"SET allow_unsigned_extensions=false",
	"SET enable_external_file_cache=false",
	"SET enable_http_metadata_cache=false",
#ifdef HAVE_DUCKDB_V2
	/* 'forbidden' also distrusts repositories created before the tightening. */
	"SET allow_extension_repositories='forbidden'",
#endif
	"SET enable_external_access=false",
	"SET lock_configuration=true",
};

/* Extensions the profile is known to contain. httpfs and excel do their I/O
 * through DuckDB's file system layer, which enable_external_access=false
 * denies. The built-ins, fts, inet, tpch, tpcds and vss do no file or network
 * I/O of their own. spatial's bundled GDAL opens its own HTTP connections
 * (/vsicurl/, WFS:, OAPIF:, ...); trusting it relies on spatial checking
 * enable_external_access against its own VSI and driver prefix list. Any other
 * loaded extension may open sockets, files or listeners directly: quack serves
 * and attaches remote DuckDBs regardless of the profile, and the scanners,
 * cloud and lakehouse extensions carry their own clients and credentials.
 * Unknown extensions are refused, not trusted. */
#define PDO_DUCKDB_SANDBOX_CONTAINED_EXTENSIONS \
	"'autocomplete', 'core_functions', 'excel', 'fts', 'httpfs', 'icu', 'inet', 'json', " \
	"'parquet', 'spatial', 'tpcds', 'tpch', 'vss'"

static void pdo_duckdb_refuse_sandbox(pdo_duckdb_db_handle *H, const char *reason)
{
	if (H->instance->sandbox_refused) {
		return;
	}
	H->instance->sandbox_refused = true;
	snprintf(H->instance->sandbox_refusal, sizeof(H->instance->sandbox_refusal),
		"Unable to apply the open_basedir sandbox profile to DuckDB: %s; "
		"the connection refuses further SQL", reason);
}

static bool pdo_duckdb_clear_instance_writers(pdo_duckdb_db_handle *H)
{
	pdd_bridge_error error = {0};
	bool ok = pdd_bridge_clear_instance_writers(H->conn, &error) == PDD_BRIDGE_OK;
	pdd_bridge_error_free(&error);
	if (!ok) {
		pdo_duckdb_refuse_sandbox(H, "Unable to stop the shared instance's file log sink");
	}
	return ok;
}

void pdo_duckdb_native_cleanup_enter(pdo_duckdb_db_handle *H)
{
	pdo_duckdb_instance_lock(H);
	if (H->instance && H->conn && PG(open_basedir) && *PG(open_basedir)) {
		pdd_bridge_error error = {0};
		(void)pdo_duckdb_clear_instance_writers(H);
		if (pdd_bridge_clear_local_writers(H->conn, &error) != PDD_BRIDGE_OK) {
			pdo_duckdb_refuse_sandbox(H, "Unable to stop a connection's file sinks before native cleanup");
		}
		pdd_bridge_error_free(&error);
	}
}

/* duckdb_extensions() reads the .info file of every extension in the extension
 * directory. An INSTALL in another process writes its .info under a temporary
 * "<name>.duckdb_extension.tmp-<uuid>.duckdb_extension.info" name, locked while
 * it is written and synced, then renames it away; the listing picks that file
 * up because of its suffix. A collision fails with an IO error naming the
 * temporary file. Retry only that error, for up to half a second (the attempt
 * cap covers a platform without a monotonic clock): a permanent error, such as
 * a corrupt .info under a regular name, must refuse without the wait. */
#define PDO_DUCKDB_EXTENSIONS_RETRY_BUDGET_NS ((uint64_t)500 * 1000 * 1000)
#define PDO_DUCKDB_EXTENSIONS_MAX_RETRIES 1000
#define PDO_DUCKDB_EXTENSIONS_RETRY_USEC 1000

static bool pdo_duckdb_extensions_install_race(duckdb_result *res)
{
	const char *err;

	if (duckdb_result_error_type(res) != DUCKDB_ERROR_IO) {
		return false;
	}
	err = duckdb_result_error(res);
	return err != NULL && strstr(err, ".duckdb_extension.tmp-") != NULL;
}

static duckdb_state pdo_duckdb_query_extensions(duckdb_connection conn, const char *sql, duckdb_result *res)
{
	uint64_t start = pdo_duckdb_hrtime_ns();
	int retries = 0;

	while (duckdb_query(conn, sql, res) != DuckDBSuccess) {
		if (!pdo_duckdb_extensions_install_race(res)
				|| ++retries > PDO_DUCKDB_EXTENSIONS_MAX_RETRIES
				|| pdo_duckdb_hrtime_ns() - start >= PDO_DUCKDB_EXTENSIONS_RETRY_BUDGET_NS) {
			return DuckDBError;
		}
		duckdb_destroy_result(res);
		memset(res, 0, sizeof(*res));
		usleep(PDO_DUCKDB_EXTENSIONS_RETRY_USEC);
	}
	return DuckDBSuccess;
}

typedef enum {
	PDO_DUCKDB_QUACK_ABSENT,
	PDO_DUCKDB_QUACK_LOADED,
	/* The extension list was unavailable, so quack may be loaded. */
	PDO_DUCKDB_QUACK_UNKNOWN
} pdo_duckdb_quack_state;

typedef struct {
	smart_str names;
	bool quack_loaded;
} pdo_duckdb_loaded_extensions;

static bool pdo_duckdb_loaded_extension_row(void *ctx, char **cells)
{
	pdo_duckdb_loaded_extensions *loaded = (pdo_duckdb_loaded_extensions *)ctx;

	if (!cells[0]) {
		return true;
	}
	if (strcmp(cells[0], "quack") == 0) {
		loaded->quack_loaded = true;
	}
	if (loaded->names.s) {
		smart_str_appends(&loaded->names, ", ");
	}
	smart_str_appends(&loaded->names, cells[0]);
	return true;
}

/* Must run while external access is still on: duckdb_extensions() lists the
 * extension directory, which the profile denies. Returns false (refusal
 * latched) if an uncontained extension is loaded or the list is unavailable;
 * *quack reports whether listeners may need stopping. */
static bool pdo_duckdb_check_loaded_extensions(pdo_duckdb_db_handle *H, duckdb_connection conn, pdo_duckdb_quack_state *quack)
{
	duckdb_result res = {0};
	pdo_duckdb_loaded_extensions loaded = {{0}, false};
	smart_str names;
	idx_t rows;
	bool listed, nul_cell;

	*quack = PDO_DUCKDB_QUACK_ABSENT;
	if (pdo_duckdb_query_extensions(conn,
			"SELECT extension_name FROM duckdb_extensions() WHERE loaded "
			"AND extension_name NOT IN (" PDO_DUCKDB_SANDBOX_CONTAINED_EXTENSIONS ") "
			"ORDER BY extension_name",
			&res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		*quack = PDO_DUCKDB_QUACK_UNKNOWN;
		pdo_duckdb_refuse_sandbox(H, "the loaded DuckDB extensions could not be listed");
		return false;
	}

	listed = pdo_duckdb_each_varchar_row(&res, 1, pdo_duckdb_loaded_extension_row, &loaded, &rows, &nul_cell);
	duckdb_destroy_result(&res);
	*quack = loaded.quack_loaded ? PDO_DUCKDB_QUACK_LOADED : PDO_DUCKDB_QUACK_ABSENT;
	names = loaded.names;
	if (!listed) {
		smart_str_free(&names);
		if (!loaded.quack_loaded) {
			*quack = PDO_DUCKDB_QUACK_UNKNOWN;
		}
		pdo_duckdb_refuse_sandbox(H, "the loaded DuckDB extensions could not be listed");
		return false;
	}

	if (rows == 0) {
		return true;
	}
	smart_str_0(&names);
	{
		char reason[200];

		snprintf(reason, sizeof(reason),
			"loaded DuckDB extensions are not contained by it (%s)",
			names.s ? ZSTR_VAL(names.s) : "unnamed");
		pdo_duckdb_refuse_sandbox(H, reason);
	}
	smart_str_free(&names);
	return false;
}

/* Best effort past every per-row failure. URIs are bound byte for byte: one
 * holding a NUL must not end the walk and leave later listeners running. */
static void pdo_duckdb_stop_quack_listeners(duckdb_connection conn)
{
	duckdb_result res = {0};
	duckdb_prepared_statement stop = NULL;
	duckdb_data_chunk chunk;

	if (duckdb_query(conn, "SELECT #1 FROM quack_server_list()", &res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return;
	}
	if (duckdb_column_count(&res) != 1 || duckdb_column_type(&res, 0) != DUCKDB_TYPE_VARCHAR
			|| duckdb_prepare(conn, "SELECT * FROM quack_stop($1)", &stop) != DuckDBSuccess) {
		duckdb_destroy_prepare(&stop);
		duckdb_destroy_result(&res);
		return;
	}

	while ((chunk = duckdb_fetch_chunk(res)) != NULL) {
		idx_t size = duckdb_data_chunk_get_size(chunk);
		duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, 0);
		duckdb_string_t *data = (duckdb_string_t *)duckdb_vector_get_data(vec);
		uint64_t *validity = duckdb_vector_get_validity(vec);
		idx_t row;

		for (row = 0; row < size; row++) {
			duckdb_result stopped = {0};

			if (validity && !duckdb_validity_row_is_valid(validity, row)) {
				continue;
			}
			if (duckdb_bind_varchar_length(stop, 1, duckdb_string_t_data(&data[row]),
					duckdb_string_t_length(data[row])) == DuckDBSuccess) {
				(void)duckdb_execute_prepared(stop, &stopped);
				duckdb_destroy_result(&stopped);
			}
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_prepare(&stop);
	duckdb_destroy_result(&res);
}

/* The refused handle never runs user SQL again, but remote clients of a quack
 * listener on the same instance still can: stop the listeners and lock down
 * what the profile does govern, ignoring failures. An unreadable .info planted
 * in the extension directory fails the list, so an unknown state still stops
 * listeners; autoloading goes off first (the profile turns it off anyway), so
 * calling quack_server_list() cannot install or load quack, and without quack
 * the call fails at bind and stops nothing. */
static void pdo_duckdb_contain_refused(duckdb_connection conn, pdo_duckdb_quack_state quack)
{
	size_t i;

	if (quack == PDO_DUCKDB_QUACK_LOADED
			|| (quack == PDO_DUCKDB_QUACK_UNKNOWN
				&& pdo_duckdb_query_ok(conn, "SET autoinstall_known_extensions=false")
				&& pdo_duckdb_query_ok(conn, "SET autoload_known_extensions=false"))) {
		pdo_duckdb_stop_quack_listeners(conn);
	}
	for (i = 0; i < sizeof(pdo_duckdb_profile_pre_detach) / sizeof(pdo_duckdb_profile_pre_detach[0]); i++) {
		(void)pdo_duckdb_query_ok(conn, pdo_duckdb_profile_pre_detach[i]);
	}
	for (i = 0; i < sizeof(pdo_duckdb_profile_post_detach) / sizeof(pdo_duckdb_profile_post_detach[0]); i++) {
		(void)pdo_duckdb_query_ok(conn, pdo_duckdb_profile_post_detach[i]);
	}
}

/* Runs on a fresh connection to the same instance: loaded extensions and quack
 * listeners are instance-wide, and a DuckDB 2.0 CONNECT on H->conn would send
 * H->conn's SQL to the remote database instead. */
static bool pdo_duckdb_admit_loaded_extensions(pdo_duckdb_db_handle *H, duckdb_connection inspect)
{
	pdo_duckdb_quack_state quack = PDO_DUCKDB_QUACK_ABSENT;
	bool ok;

	ok = pdo_duckdb_check_loaded_extensions(H, inspect, &quack);
	if (!ok) {
		pdo_duckdb_contain_refused(inspect, quack);
	}
	return ok;
}

/* Disable DuckDB external access (read_csv/COPY/ATTACH/httpfs) on the shared
 * instance. Order matters:
 * 0. Stop instance logging before creating the inspector, then local writers
 *    without SQL: SQL could dispatch remotely or end a live result and flush
 *    its profiling file before cleanup takes effect.
 * 1. Refuse if a loaded extension escapes the profile; it cannot be
 *    unloaded, so the profile alone would leave its network access open.
 * 2. Neutralize instance writers and OnSet re-allowlist inputs (empty temp,
 *    out-of-basedir ATTACHes) while external access is still on.
 * 3. Clear allowlists before flipping enable_external_access: DuckDB refuses
 *    allowed_directories/allowed_paths changes after external access is off, but
 *    pre-existing allowlists remain effective.
 * 4. enable_external_access=false runs DuckDB's OnSet (empty temp seeds nothing).
 * 5. lock_configuration last so none of the above can be undone. */
static bool pdo_duckdb_apply_instance_sandbox(pdo_duckdb_db_handle *H, duckdb_connection inspect)
{
	size_t i;
	pdo_duckdb_db_handle *member;
	bool local_ok = true;
	pdd_bridge_error error = {0};

	/* Sinks belong to individual ClientContexts, but the configuration lock
	 * belongs to their shared DatabaseInstance. Attempt every local sink even
	 * if one cleanup fails, including constructor defaults on the inspector. */
	for (member = H->instance->handles; member; member = member->instance_next) {
		if (pdd_bridge_clear_local_writers(member->conn, &error) != PDD_BRIDGE_OK) {
			local_ok = false;
		}
		pdd_bridge_error_free(&error);
	}
	if (pdd_bridge_clear_local_writers(inspect, &error) != PDD_BRIDGE_OK) {
		local_ok = false;
	}
	pdd_bridge_error_free(&error);
	if (!local_ok) {
		pdo_duckdb_refuse_sandbox(H, "Unable to stop a shared connection's file sinks");
		return false;
	}
	if (!pdo_duckdb_admit_loaded_extensions(H, inspect)) {
		return false;
	}
	for (i = 0; i < sizeof(pdo_duckdb_profile_pre_detach) / sizeof(pdo_duckdb_profile_pre_detach[0]); i++) {
		if (!pdo_duckdb_query_ok(inspect, pdo_duckdb_profile_pre_detach[i])) {
			pdo_duckdb_refuse_sandbox(H, "Unable to apply the shared open_basedir sandbox");
			return false;
		}
	}
	if (!pdo_duckdb_detach_out_of_basedir(H, inspect)) {
		pdo_duckdb_refuse_sandbox(H, "Unable to detach databases outside open_basedir");
		return false;
	}
	for (i = 0; i < sizeof(pdo_duckdb_profile_post_detach) / sizeof(pdo_duckdb_profile_post_detach[0]); i++) {
		if (!pdo_duckdb_query_ok(inspect, pdo_duckdb_profile_post_detach[i])) {
			pdo_duckdb_refuse_sandbox(H, "Unable to lock the shared open_basedir sandbox");
			return false;
		}
	}
	H->instance->external_access_disabled = true;
	/* Only reached from enforce_sandbox, which has checked that open_basedir is
	 * non-empty. */
	H->instance->sandbox_basedir = strdup(PG(open_basedir));
	if (!H->instance->sandbox_basedir) {
		pdo_duckdb_refuse_sandbox(H, "Unable to record the shared open_basedir sandbox");
		return false;
	}
	return true;
}

static bool pdo_duckdb_disable_external_access(pdo_duckdb_db_handle *H)
{
	duckdb_connection inspect = NULL;
	bool allowed = false;

	if (!pdo_duckdb_clear_instance_writers(H)) {
		return false;
	}
	if (duckdb_connect(H->db, &inspect) != DuckDBSuccess) {
		pdo_duckdb_refuse_sandbox(H, "the loaded DuckDB extensions could not be listed");
		return false;
	}
	zend_try {
		allowed = pdo_duckdb_apply_instance_sandbox(H, inspect);
	} zend_catch {
		duckdb_disconnect(&inspect);
		zend_bailout();
	} zend_end_try();
	duckdb_disconnect(&inspect);
	return allowed;
}

static bool pdo_duckdb_enforce_sandbox_locked(pdo_duckdb_db_handle *H)
{
	if (!H->instance || H->instance->sandbox_refused) {
		return false;
	}
	if (!(PG(open_basedir) && *PG(open_basedir))) {
		return true;
	}
	if (!H->instance->external_access_disabled) {
		return pdo_duckdb_disable_external_access(H);
	}
	/* Already sandboxed: DuckDB's allowlists are frozen, so they cannot track a
	 * re-narrowed basedir. Anything but the recorded value fails closed. */
	return H->instance->sandbox_basedir != NULL && strcmp(PG(open_basedir), H->instance->sandbox_basedir) == 0;
}

bool pdo_duckdb_native_enter(pdo_duckdb_db_handle *H)
{
	bool allowed = false;
	pdo_duckdb_instance_lock(H);
	zend_try {
		allowed = pdo_duckdb_enforce_sandbox_locked(H);
	} zend_catch {
		if (H->instance) {
			pdo_duckdb_refuse_sandbox(H, "The shared sandbox transition was interrupted");
		}
		pdo_duckdb_instance_unlock(H);
		zend_bailout();
	} zend_end_try();
	if (!allowed) {
		pdo_duckdb_instance_unlock(H);
	}
	return allowed;
}

bool pdo_duckdb_enforce_sandbox(pdo_duckdb_db_handle *H)
{
	if (!pdo_duckdb_native_enter(H)) {
		return false;
	}
	pdo_duckdb_instance_unlock(H);
	return true;
}

const char *pdo_duckdb_sandbox_error(pdo_duckdb_db_handle *H)
{
	pdo_duckdb_instance_lock(H);
	snprintf(H->sandbox_error, sizeof(H->sandbox_error), "%s",
		H->instance && H->instance->sandbox_refused ? H->instance->sandbox_refusal
		: "Unable to apply the open_basedir sandbox profile to DuckDB");
	pdo_duckdb_instance_unlock(H);
	return H->sandbox_error;
}

/* Persistent handles skip handle_factory on reuse, so PDO calls check_liveness
 * on every reuse. Escalate the sandbox there. A FAILURE rebuild can leak the
 * discarded persistent dbh in PDO core (8.1–8.3; 8.4+ only when refcount > 1),
 * so tear our DuckDB instance down first when we are the last holder. */
static const char *const pdo_duckdb_liveness_network_resets[] = {
	"RESET http_proxy",
	"RESET http_proxy_username",
	"RESET http_proxy_password",
};

static const char *const pdo_duckdb_liveness_httpfs_resets[] = {
	"RESET enable_curl_server_cert_verification",
	"RESET enable_server_cert_verification",
	"RESET ca_cert_file",
	"RESET httpfs_client_implementation",
};

/* Tri-state httpfs probe: 1 when loaded, 0 when not, -1 when the probe query
 * itself failed (a sick connection, not an unloaded extension). */
static int pdo_duckdb_httpfs_loaded_state(duckdb_connection conn)
{
	duckdb_result res = {0};
	int state;

	if (duckdb_query(conn,
			"SELECT 1 FROM duckdb_extensions() "
			"WHERE extension_name = 'httpfs' AND loaded",
			&res) != DuckDBSuccess) {
		duckdb_destroy_result(&res);
		return -1;
	}
	state = pdo_duckdb_result_has_rows(&res) ? 1 : 0;
	if (duckdb_result_error(&res) != NULL) {
		state = -1;
	}
	duckdb_destroy_result(&res);
	return state;
}

static void pdo_duckdb_liveness_discard(pdo_dbh_t *dbh)
{
	if (dbh->refcount <= 1) {
		duckdb_handle_closer(dbh);
	}
}

#ifdef HAVE_DUCKDB_V2
/* Detect CONNECT without dispatching SQL or ending an active result. */
static bool pdo_duckdb_session_is_local(duckdb_connection conn)
{
	pdd_bridge_error error = {0};
	bool remote = false;
	bool local = pdd_bridge_connection_is_remote(conn, &remote, &error) == PDD_BRIDGE_OK && !remote;
	pdd_bridge_error_free(&error);
	return local;
}
#endif

static zend_result pdo_duckdb_check_liveness(pdo_dbh_t *dbh)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	size_t i;
	/* A failed residue reset means the connection may carry another
	 * request's session state (or be sick); take the discard path rather
	 * than silently reusing it. */
	bool dirty = false;

	if (!pdo_duckdb_native_enter(H)) {
		pdo_duckdb_liveness_discard(dbh);
		return FAILURE;
	}

	/* Residue SQL is for a true request-boundary checkout (list-only handle).
	 * A second live PDO wrapper (refcount > 1) may be mid-transaction; a
	 * failed RESET there aborts it. */
	if (dbh->refcount <= 1) {
#ifdef HAVE_DUCKDB_V2
		/* The residue SQL below would reach the remote database, and the next
		 * request would inherit the binding. */
		if (!pdo_duckdb_session_is_local(H->conn)) {
			pdo_duckdb_instance_unlock(H);
			pdo_duckdb_liveness_discard(dbh);
			return FAILURE;
		}
#endif
		if (!H->instance->external_access_disabled) {
			for (i = 0; i < sizeof(pdo_duckdb_liveness_network_resets)
					/ sizeof(pdo_duckdb_liveness_network_resets[0]); i++) {
				if (!pdo_duckdb_query_ok(H->conn, pdo_duckdb_liveness_network_resets[i])) {
					dirty = true;
				}
			}
			/* httpfs options autoload the extension on RESET if it
			 * isn't loaded; only touch them when a prior request
			 * already did. */
			{
				int httpfs = pdo_duckdb_httpfs_loaded_state(H->conn);
				if (httpfs < 0) {
					dirty = true;
				} else if (httpfs > 0) {
					for (i = 0; i < sizeof(pdo_duckdb_liveness_httpfs_resets)
							/ sizeof(pdo_duckdb_liveness_httpfs_resets[0]); i++) {
						if (!pdo_duckdb_query_ok(H->conn, pdo_duckdb_liveness_httpfs_resets[i])) {
							dirty = true;
						}
					}
				}
			}
		}
		/* A live wrapper may own a streaming result. Residue cleanup belongs
		 * to request-boundary checkout; executing SQL here would end that result. */
		if (!pdo_duckdb_query_ok(H->conn, "PRAGMA disable_profiling")) {
			dirty = true;
		}
	}
	pdo_duckdb_instance_unlock(H);

	H->unbuffered = false;
	H->native_structures = false;
	pdo_duckdb_clear_einfo(&H->einfo, dbh->is_persistent);
	if (dirty) {
		pdo_duckdb_liveness_discard(dbh);
		return FAILURE;
	}
	return SUCCESS;
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
		case PDO_DUCKDB_ATTR_NATIVE_STRUCTURES:
			ZVAL_BOOL(return_value, ((pdo_duckdb_db_handle *)dbh->driver_data)->native_structures);
			break;

		default:
			return 0;
	}

	return 1;
}

/* Omit newer PDO fields so this table also builds on older PHP versions. */
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

/* Returns an emalloc'd path, or NULL for in-memory ("" / ":memory:"). On
 * failure returns NULL with *deny_reason set; an unresolvable path and an
 * open_basedir denial get distinct messages. */
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

/* On an invalid name/value, records a CONNECT error and destroys the config. */
static bool pdo_duckdb_set_one_config(pdo_dbh_t *dbh, duckdb_config *config, const char *key, const char *value)
{
	size_t key_len = strlen(key);

	/* DuckDB 1.5.3–1.5.6 dereference a NULL DatabaseInstance for a falsy
	 * force_mbedtls_unsafe at config-time (SIGSEGV inside duckdb_set_config).
	 * SET after open still works. */
	if (key_len == sizeof("force_mbedtls_unsafe") - 1
			&& zend_binary_strcasecmp(key, key_len, "force_mbedtls_unsafe",
				sizeof("force_mbedtls_unsafe") - 1) == 0) {
		duckdb_destroy_config(config);
		*config = NULL;
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
			"DuckDB configuration option \"force_mbedtls_unsafe\" cannot be set at connect time");
		return false;
	}

	if (duckdb_set_config(*config, key, value) != DuckDBSuccess) {
		duckdb_destroy_config(config);
		*config = NULL;
		/* Name the key but never the value: DSN option tails and array values
		 * can carry secrets. */
		char *msg;
		spprintf(&msg, 0, "Invalid DuckDB configuration option \"%s\"", key);
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
		efree(msg);
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
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allowed_configs") ||
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
#ifdef HAVE_DUCKDB_V2
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "allow_extension_repositories") ||
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "extension_repository_directory") ||
#endif
		PDO_DUCKDB_CONFIG_KEY_MATCHES(key, key_len, "secret_directory");
}

#undef PDO_DUCKDB_CONFIG_KEY_MATCHES

static bool pdo_duckdb_reject_sandbox_config_key(pdo_dbh_t *dbh, duckdb_config *config, const char *key, size_t key_len)
{
	if (pdo_duckdb_sandbox_forbids_config_key(key, key_len)) {
		if (*config) {
			duckdb_destroy_config(config);
		}
		char *msg;
		spprintf(&msg, 0,
			"DuckDB configuration option \"%.*s\" is not allowed when open_basedir is set",
			(int)key_len, key);
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
		efree(msg);
		return false;
	}
	return true;
}

/* Open-time flags only. Do not set enable_external_access=false or
 * lock_configuration here: DuckDB Configure would permanently allowlist the
 * default temp dir, and lock would prevent the post-connect escalate cleaner
 * (pdo_duckdb_disable_external_access) from clearing it. */
static bool pdo_duckdb_apply_sandbox_config(pdo_dbh_t *dbh, duckdb_config *config)
{
	static const char *const sandbox_options[][2] = {
		{"temp_directory", ""},
		{"autoinstall_known_extensions", "false"},
		{"autoload_known_extensions", "false"},
		{"allow_community_extensions", "false"},
		{"allow_extensions_metadata_mismatch", "false"},
		{"allow_persistent_secrets", "false"},
		{"allow_unredacted_secrets", "false"},
		{"allow_unsigned_extensions", "false"},
		{"enable_external_file_cache", "false"},
		{"enable_http_metadata_cache", "false"},
#ifdef HAVE_DUCKDB_V2
		{"allow_extension_repositories", "forbidden"},
#endif
	};
	size_t i;

	for (i = 0; i < sizeof(sandbox_options) / sizeof(sandbox_options[0]); i++) {
		if (duckdb_set_config(*config, sandbox_options[i][0], sandbox_options[i][1]) != DuckDBSuccess) {
			duckdb_destroy_config(config);
			*config = NULL;
			char *msg;
			spprintf(&msg, 0, "Unable to apply the open_basedir sandbox (%s) to DuckDB",
				sandbox_options[i][0]);
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
			efree(msg);
			return false;
		}
	}
	return true;
}

static const char *pdo_duckdb_open_error_message(const char *open_error)
{
	/* These complete diagnostics contain no path. A short relative filename
	 * such as "alias" or "configuration" must not erase their explanation. */
	if (open_error && (
			strcmp(open_error, "A hard-link alias of this database file is already open; use the original database path") == 0 ||
			strcmp(open_error, "DuckDB cannot safely open distinct database paths that differ only by case simultaneously") == 0 ||
			strcmp(open_error, "The database path changed while another connection still uses it") == 0 ||
			strcmp(open_error, "Unable to verify the database file identity safely") == 0 ||
			strcmp(open_error, "Connection Error: Can't open a connection to same database file with a different configuration than existing connections") == 0)) {
		return open_error;
	}
	if (open_error && strstr(open_error, "options were not recognized:")) {
		return open_error;
	}
	return "Unable to open DuckDB database";
}

static zend_string *pdo_duckdb_config_scalar_to_string(pdo_dbh_t *dbh, zval *value)
{
	ZVAL_DEREF(value);

	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			return ZSTR_EMPTY_ALLOC();
		case IS_FALSE:
			return zend_string_init("false", sizeof("false") - 1, 0);
		case IS_TRUE:
			return zend_string_init("true", sizeof("true") - 1, 0);
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
			return zval_get_string(value);
		default: {
			char *msg;
			spprintf(&msg, 0,
				"PDO::DUCKDB_ATTR_CONFIG values must be scalar or null, %s given",
				zend_zval_type_name(value));
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
			efree(msg);
			return NULL;
		}
	}
}

/* Applied in order so the sandbox wins: DSN "key=value;..." tail, then
 * PDO::DUCKDB_ATTR_CONFIG, then (under open_basedir) rejection of
 * path-sensitive options plus the open-time sandbox flags. External access and
 * the lock are applied post-connect so file-DB temp allowlists can still be
 * cleared. *out_config is NULL when nothing needs setting. Returns false with an
 * exception thrown on a bad option. */
static bool pdo_duckdb_build_config(pdo_dbh_t *dbh, const char *dsn_opts,
		zval *driver_options, duckdb_config *out_config)
{
	duckdb_config config = NULL;
	bool sandbox = (PG(open_basedir) && *PG(open_basedir));

	*out_config = NULL;

#define DUCKDB_ENSURE_CONFIG() do { \
		if (!config) { \
			if (duckdb_create_config(&config) != DuckDBSuccess) { \
				pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, \
					"Unable to allocate DuckDB configuration"); \
				return false; \
			} \
		} \
	} while (0)

	if (dsn_opts && *dsn_opts) {
		char *copy = estrdup(dsn_opts);
		char *save = NULL;
		char *pair = php_strtok_r(copy, ";", &save);

		while (pair) {
			char *eq = strchr(pair, '=');
			if (eq && eq != pair) {
				*eq = '\0';
				if (sandbox && !pdo_duckdb_reject_sandbox_config_key(dbh, &config, pair, strlen(pair))) {
					efree(copy);
					return false;
				}
				DUCKDB_ENSURE_CONFIG();
				if (!pdo_duckdb_set_one_config(dbh, &config, pair, eq + 1)) {
					efree(copy);
					return false;
				}
			} else if (*pair) {
				/* Don't echo the raw segment: DSN option tails can contain secrets. */
				if (config) {
					duckdb_destroy_config(&config);
				}
				pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
					"Malformed DuckDB DSN option (expected key=value)");
				efree(copy);
				return false;
			}
			pair = php_strtok_r(NULL, ";", &save);
		}
		efree(copy);
	}

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
				pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
					"PDO::DUCKDB_ATTR_CONFIG must be an array of option => value");
				return false;
			}

			ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(cfg), key, val) {
				zend_string *sval;

				if (!key) {
					if (config) {
						duckdb_destroy_config(&config);
					}
					pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
						"PDO::DUCKDB_ATTR_CONFIG keys must be option-name strings");
					return false;
				}
				/* duckdb_set_config() would truncate at an embedded NUL and apply a
				 * different option than requested. */
				if (zend_str_has_nul_byte(key)) {
					if (config) {
						duckdb_destroy_config(&config);
					}
					pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
						"PDO::DUCKDB_ATTR_CONFIG option names and values must not contain a NUL byte");
					return false;
				}
				sval = pdo_duckdb_config_scalar_to_string(dbh, val);
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
					pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
						"PDO::DUCKDB_ATTR_CONFIG option names and values must not contain a NUL byte");
					return false;
				}
				if (sandbox && !pdo_duckdb_reject_sandbox_config_key(dbh, &config, ZSTR_VAL(key), ZSTR_LEN(key))) {
					zend_string_release(sval);
					return false;
				}
				DUCKDB_ENSURE_CONFIG();
				if (!pdo_duckdb_set_one_config(dbh, &config, ZSTR_VAL(key), ZSTR_VAL(sval))) {
					zend_string_release(sval);
					return false;
				}
				zend_string_release(sval);
			} ZEND_HASH_FOREACH_END();
		}
	}

	/* Applied last so it overrides user settings; path-sensitive settings were
	 * rejected above. Fails closed. */
	if (sandbox) {
		DUCKDB_ENSURE_CONFIG();
		if (!pdo_duckdb_apply_sandbox_config(dbh, &config)) {
			return false;
		}
	}
	DUCKDB_ENSURE_CONFIG();

#undef DUCKDB_ENSURE_CONFIG

	*out_config = config;
	return true;
}

static int pdo_duckdb_handle_factory_impl(pdo_dbh_t *dbh, zval *driver_options) /* {{{ */
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
	H->persistent = dbh->is_persistent;
	H->config_reapply_pending = pdo_duckdb_driver_options_has_config(driver_options);
	dbh->driver_data = H;

	if (dbh->is_persistent && H->config_reapply_pending) {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT,
			"PDO::DUCKDB_ATTR_CONFIG cannot be used with persistent DuckDB connections");
		goto cleanup;
	}

	semi = strchr(dbh->data_source, ';');
	if (semi) {
		path_dsn = estrndup(dbh->data_source, semi - dbh->data_source);
		dsn_opts = semi + 1;
	}

	display_source = path_dsn ? path_dsn : dbh->data_source;
	path = duckdb_make_path_safe(display_source, &deny_reason);
	if (deny_reason) {
		char *msg;
		spprintf(&msg, 0, "Cannot open DuckDB database %s: %s", display_source, deny_reason);
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
		efree(msg);
		goto cleanup;
	}
	/* path_dsn stays alive for the open_error taint check below; freed at cleanup. */

	if (!pdo_duckdb_build_config(dbh, dsn_opts, driver_options, &config)) {
		if (path) {
			efree(path);
		}
		goto cleanup;  /* exception already thrown */
	}
	if (driver_options && Z_TYPE_P(driver_options) == IS_ARRAY) {
		zval *unbuf = zend_hash_index_find(Z_ARRVAL_P(driver_options), PDO_DUCKDB_ATTR_UNBUFFERED);
		if (unbuf) {
			H->unbuffered = pdo_duckdb_zval_is_true(unbuf);
		}
		zval *native = zend_hash_index_find(Z_ARRVAL_P(driver_options), PDO_DUCKDB_ATTR_NATIVE_STRUCTURES);
		if (native) {
			H->native_structures = pdo_duckdb_zval_is_true(native);
		}
	}

	open_state = pdo_duckdb_open_instance(H, path, config, &open_error);
	if (config) {
		duckdb_destroy_config(&config);
	}
	if (open_state != DuckDBSuccess) {
		const char *base = pdo_duckdb_open_error_message(open_error);
		/* Append DuckDB's detail unless it echoes the database path, which must
		 * stay hidden. DuckDB never sees the DSN option tail, so checking for
		 * the path is enough. */
		bool tainted = false;
		if (open_error && base != open_error) {
			if (path && *path && strstr(open_error, path)) {
				tainted = true;
			}
			if (!tainted && display_source && *display_source
					&& strcmp(display_source, ":memory:") != 0
					&& strstr(open_error, display_source)) {
				tainted = true;
			}
		}
		if (!tainted && open_error && base != open_error) {
			char *msg;
			spprintf(&msg, 0, "%s: %s", base, open_error);
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, msg);
			efree(msg);
		} else {
			pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_CONNECT, base);
		}
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

	/* Full SQL sandbox after connect so allowlists seeded at open (e.g. default
	 * temp for file DBs) can still be cleared before external access is locked. */
	if (!pdo_duckdb_enforce_sandbox(H)) {
		pdo_duckdb_error_code(dbh, PDO_DUCKDB_ERRCODE_SANDBOX, pdo_duckdb_sandbox_error(H));
		goto cleanup;
	}

	dbh->alloc_own_columns = 1;
	dbh->max_escaped_char_length = 2;

	ret = 1;

cleanup:
	if (path_dsn) {
		efree(path_dsn);
	}
	dbh->methods = &duckdb_methods;
	return ret;
}
/* }}} */

/* {{{ Driver-specific methods, shared by the Pdo\Duckdb subclass (8.4+) and the
 * base-PDO get_driver_methods path (8.1-8.3). */

/* DuckDB's qualified names append the alias ("s.orders AS o"). An unquoted
 * identifier can't contain a space, so a top-level " AS " is always the alias
 * separator. */
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

/* duckdb_get_table_names() binds the target of UPDATE, DELETE, and MERGE as a
 * dummy scan that the statement binder then rejects as not a base table, so it
 * returns NULL just as for a parse failure. A successful prepare() tells such a
 * valid statement apart from a broken one. */
static bool pdo_duckdb_is_dml_without_table_names(pdo_duckdb_db_handle *H, const char *query)
{
	duckdb_prepared_statement probe = NULL;
	bool dml = false;

	if (duckdb_prepare(H->conn, query, &probe) == DuckDBSuccess) {
		switch (duckdb_prepared_statement_type(probe)) {
			case DUCKDB_STATEMENT_TYPE_UPDATE:
			case DUCKDB_STATEMENT_TYPE_DELETE:
#if !defined(HAVE_DECL_DUCKDB_STATEMENT_TYPE_MERGE_INTO) || HAVE_DECL_DUCKDB_STATEMENT_TYPE_MERGE_INTO
			case DUCKDB_STATEMENT_TYPE_MERGE_INTO:
#endif
				dml = true;
				break;
			default:
				break;
		}
	}
	if (probe) {
		duckdb_destroy_prepare(&probe);
	}
	return dml;
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

	/* duckdb_get_table_names() would truncate at an embedded NUL. */
	if (zend_str_has_nul_byte(query)) {
		zend_value_error("PDO::duckdbTableNames(): query must not contain a NUL byte");
		RETURN_THROWS();
	}

	/* Catalog binding can touch files even without execution. */
	if (!pdo_duckdb_native_enter(H)) {
		zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SANDBOX,
			"PDO::duckdbTableNames(): %s",
			pdo_duckdb_sandbox_error(H));
		RETURN_THROWS();
	}

	list = duckdb_get_table_names(H->conn, ZSTR_VAL(query), qualified);
	if (!list) {
		bool dml = pdo_duckdb_is_dml_without_table_names(H, ZSTR_VAL(query));

		pdo_duckdb_instance_unlock(H);
		if (dml) {
			array_init(return_value);
			return;
		}
		/* NULL means the query failed to parse or bind; get_table_names exposes
		 * no error detail, so prepare() the query for the specific message. */
		zend_throw_exception_ex(php_pdo_get_exception(), PDO_DUCKDB_ERRCODE_SYNTAX,
			"PDO::duckdbTableNames(): could not parse the query");
		RETURN_THROWS();
	}
	pdo_duckdb_instance_unlock(H);

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

/* Shape: ['metrics' => array<string, string|null>, 'children' => list]. */
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
			/* duckdb_get_varchar() aborts the process on a NULL value. */
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

	/* NULL when profiling was never enabled. */
	pdo_duckdb_instance_lock(H);
	info = duckdb_get_profiling_info(H->conn);
	if (!info) {
		pdo_duckdb_instance_unlock(H);
		RETURN_NULL();
	}
	zend_try {
		pdo_duckdb_build_profile_node(info, return_value);
	} zend_catch {
		pdo_duckdb_instance_unlock(H);
		zend_bailout();
	} zend_end_try();
	pdo_duckdb_instance_unlock(H);
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

static int pdo_duckdb_handle_factory(pdo_dbh_t *dbh, zval *driver_options)
{
	int result = 0;

	/* A request bailout can occur before PDO installs the factory's methods.
	 * Remove the handle from the process-wide registry before Zend frees it. */
	zend_try {
		result = pdo_duckdb_handle_factory_impl(dbh, driver_options);
	} zend_catch {
		duckdb_handle_closer(dbh);
		zend_bailout();
	} zend_end_try();
	return result;
}

const pdo_driver_t pdo_duckdb_driver = {
	PDO_DRIVER_HEADER(duckdb),
	pdo_duckdb_handle_factory
};
