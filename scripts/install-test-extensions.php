<?php
// Pre-install the DuckDB extensions the sandbox and SQL compatibility tests load, so a
// failed download fails CI here instead of turning those tests into SKIPs.
//
// Usage: php -d extension=modules/pdo_duckdb.so scripts/install-test-extensions.php DIR
// Then export PDO_DUCKDB_TEST_EXTENSION_DIR=DIR for run-tests.php.
if ($argc !== 2) {
    fwrite(STDERR, "usage: install-test-extensions.php DIR\n");
    exit(2);
}
if (!is_dir($argv[1]) && !mkdir($argv[1], 0777, true)) {
    fwrite(STDERR, "cannot create {$argv[1]}\n");
    exit(1);
}
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET extension_directory=' . $db->quote($argv[1]));
require dirname(__DIR__) . '/tests/duckdb_v2.inc';
$extensions = ['httpfs', 'quack', 'inet', 'spatial', 'sqlite_scanner'];
if (pdo_duckdb_v2_headers()) {
    $extensions[] = 'legacy_parser';
}
foreach ($extensions as $name) {
    $db->exec("INSTALL $name");
    $db->exec("LOAD $name");
    echo "installed and loaded $name\n";
}
