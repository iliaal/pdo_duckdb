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
#ifndef HAVE_PDO_DUCKDB_BRIDGE
# error "pdo_duckdb requires the verified exact-source DuckDB bridge"
#endif
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include "duckdb_static_extensions.h"
#include "pdo_duckdb_bridge.h"
#include "zend_exceptions.h"

/* {{{ pdo_duckdb_deps */
static const zend_module_dep pdo_duckdb_deps[] = {
	ZEND_MOD_REQUIRED("pdo")
	ZEND_MOD_END
};
/* }}} */

/* {{{ pdo_duckdb_module_entry */
zend_module_entry pdo_duckdb_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL,
	pdo_duckdb_deps,
	"pdo_duckdb",
	NULL,
	PHP_MINIT(pdo_duckdb),
	PHP_MSHUTDOWN(pdo_duckdb),
	PHP_RINIT(pdo_duckdb),
	PHP_RSHUTDOWN(pdo_duckdb),
	PHP_MINFO(pdo_duckdb),
	PHP_PDO_DUCKDB_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#if defined(COMPILE_DL_PDO_DUCKDB) || defined(COMPILE_DL_PDO_DUCKDB_EXTERNAL)
ZEND_GET_MODULE(pdo_duckdb)
#endif

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(pdo_duckdb)
{
	zend_class_entry *pdo_ce;
	pdd_bridge_error error = {0};

	if (pdd_bridge_check(&error) != PDD_BRIDGE_OK) {
		char message[256];
		if (error.data) {
			snprintf(message, sizeof(message), "%.*s", (int)MIN(error.length, sizeof(message) - 1), error.data);
		} else {
			snprintf(message, sizeof(message), "Unable to verify the DuckDB bridge header/library identity");
		}
		pdd_bridge_error_free(&error);
		php_error_docref(NULL, E_WARNING, "%s", message);
		return FAILURE;
	}
	pdd_bridge_error_free(&error);

	if (pdo_duckdb_register_static_extensions() != 0) {
		php_error_docref(NULL, E_WARNING, "Unable to register bundled DuckDB extensions");
		return FAILURE;
	}

	if (php_pdo_register_driver(&pdo_duckdb_driver) == FAILURE) {
		return FAILURE;
	}
	if (pdo_duckdb_instances_minit() == FAILURE) {
		php_pdo_unregister_driver(&pdo_duckdb_driver);
		return FAILURE;
	}

	/* Base-PDO constants also work before the PHP 8.4 driver subclass API. */
	pdo_ce = php_pdo_get_dbh_ce();
	zend_declare_class_constant_long(pdo_ce,
		ZEND_STRL("DUCKDB_ATTR_CONFIG"), PDO_DUCKDB_ATTR_CONFIG);
	zend_declare_class_constant_long(pdo_ce,
		ZEND_STRL("DUCKDB_ATTR_UNBUFFERED"), PDO_DUCKDB_ATTR_UNBUFFERED);
	zend_declare_class_constant_long(pdo_ce,
		ZEND_STRL("DUCKDB_ATTR_NATIVE_STRUCTURES"), PDO_DUCKDB_ATTR_NATIVE_STRUCTURES);

	if (pdo_duckdb_appender_minit() == FAILURE) {
		/* Don't leave a half-initialized module: PDO would keep a "duckdb" driver
		 * entry for a module that reported startup FAILURE. */
		php_pdo_unregister_driver(&pdo_duckdb_driver);
		pdo_duckdb_instances_mshutdown();
		return FAILURE;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(pdo_duckdb)
{
	php_pdo_unregister_driver(&pdo_duckdb_driver);
	pdo_duckdb_tls_caches_shutdown();
	pdo_duckdb_instances_mshutdown();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(pdo_duckdb)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(pdo_duckdb)
{
	pdo_duckdb_tls_caches_shutdown();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(pdo_duckdb)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "PDO Driver for DuckDB", "enabled");
	php_info_print_table_row(2, "DuckDB Library", duckdb_library_version());
#ifdef HAVE_DUCKDB_V2
	php_info_print_table_row(2, "DuckDB 2.0 headers", "yes");
#endif
#if defined(HAVE_DUCKDB_V2) && defined(PDO_DUCKDB_TEST_UNDECODABLE_TYPE)
	php_info_print_table_row(2, "Test-only undecodable type id", ZEND_TOSTR(PDO_DUCKDB_TEST_UNDECODABLE_TYPE));
#endif
	php_info_print_table_end();
}
/* }}} */
