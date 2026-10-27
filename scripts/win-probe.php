<?php
// Runtime smoke probe for the Windows prebuilt path: prove a freshly built
// php_pdo_duckdb.dll loads and queries with duckdb.dll resolved from the PHP
// root (where PIE installs bundled dependency DLLs). Run as:
//   php -d extension=<path>\php_pdo_duckdb.dll scripts\win-probe.php
require __DIR__ . '/binary-probe.php';
