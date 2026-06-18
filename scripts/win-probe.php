<?php
// Runtime smoke probe for the Windows prebuilt path: prove a freshly built
// php_pdo_duckdb.dll loads and queries with duckdb.dll resolved from the PHP
// root (where PIE installs bundled dependency DLLs). Run as:
//   php -d extension=<path>\php_pdo_duckdb.dll scripts\win-probe.php
$d = new PDO("duckdb::memory:");
$d->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$v = $d->getAttribute(PDO::ATTR_CLIENT_VERSION);
$d->exec("CREATE TABLE t(i INTEGER, s VARCHAR)");
$d->exec("INSERT INTO t VALUES (1,'a'),(2,'b')");
$n = (int) $d->query("SELECT count(*) FROM t")->fetchColumn();
if ($n !== 2) {
    fwrite(STDERR, "PROBE-FAIL: bad count $n\n");
    exit(1);
}
echo "PROBE-OK: bundled-DLL query works, duckdb $v\n";
