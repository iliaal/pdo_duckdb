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
	NULL,
	NULL,
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
	if (php_pdo_register_driver(&pdo_duckdb_driver) == FAILURE) {
		return FAILURE;
	}
	return pdo_duckdb_appender_minit();
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(pdo_duckdb)
{
	php_pdo_unregister_driver(&pdo_duckdb_driver);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(pdo_duckdb)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "PDO Driver for DuckDB", "enabled");
	php_info_print_table_row(2, "DuckDB Library", duckdb_library_version());
	php_info_print_table_end();
}
/* }}} */
