--TEST--
pdo_duckdb: open_basedir tightened after a (non-persistent) handle opens still sandboxes it
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// No open_basedir at open time, so the handle opens with external access on.
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->query('SELECT 1')->fetchColumn();

// Tighten open_basedir at runtime; the already-open handle must be sandboxed
// on the next SQL it runs (escalated at the preparer/doer entry points).
ini_set('open_basedir', __DIR__);

try {
    $db->query("SELECT * FROM read_csv('/etc/hostname')")->fetchAll();
    echo "BYPASS via query\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}
try {
    $db->exec("CREATE TABLE x AS SELECT * FROM read_csv('/etc/hostname')");
    echo "BYPASS via exec\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}

// Plain SQL still works.
var_dump((int) $db->query('SELECT 42')->fetchColumn());
?>
--EXPECT--
bool(true)
bool(true)
int(42)
