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

#ifndef PHP_PDO_DUCKDB_H
#define PHP_PDO_DUCKDB_H

extern zend_module_entry pdo_duckdb_module_entry;
#define phpext_pdo_duckdb_ptr &pdo_duckdb_module_entry

#define PHP_PDO_DUCKDB_VERSION "0.4.1"

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(pdo_duckdb);
PHP_MSHUTDOWN_FUNCTION(pdo_duckdb);
PHP_MINFO_FUNCTION(pdo_duckdb);

#endif /* PHP_PDO_DUCKDB_H */
