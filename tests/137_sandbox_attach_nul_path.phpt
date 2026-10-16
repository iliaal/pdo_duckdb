--TEST--
pdo_duckdb: an attached path holding a NUL byte latches a specific sandbox refusal
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
// DuckDB 1.5 only takes a string literal as the ATTACH path.
if (!pdo_duckdb_v2_headers()) {
    die('skip needs a build against the DuckDB 2.0 headers');
}
?>
--FILE--
<?php
$path = __DIR__ . '/137_attach_' . getmypid() . '.db';
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('ATTACH (' . $db->quote($path) . ' || chr(0)) AS n');
var_dump((int) $db->query("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'n' AND contains(path, chr(0))")->fetchColumn());

ini_set('open_basedir', __DIR__);

foreach (['SELECT 1', 'SELECT 2'] as $sql) {
    try {
        $db->query($sql)->fetchAll();
        echo "$sql: ALLOWED\n";
    } catch (PDOException $e) {
        echo "$sql: ", str_contains($e->getMessage(), 'an attached database name or path contains a NUL byte') ? 'refused with reason' : 'other error: ' . $e->getMessage(), "\n";
    }
}
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/137_attach_*') as $f) {
    @unlink($f);
}
?>
--EXPECT--
int(1)
SELECT 1: refused with reason
SELECT 2: refused with reason
