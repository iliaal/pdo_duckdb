--TEST--
pdo_duckdb: a persistent handle can't bypass open_basedir tightened after it was cached
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Open a PERSISTENT handle with no open_basedir in effect (external SQL file
// access is enabled at this point).
$a = new PDO('duckdb::memory:', null, null, [PDO::ATTR_PERSISTENT => true]);
$a->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
unset($a); // back to the persistent pool, still alive

// Tighten open_basedir at runtime (PHP only allows narrowing).
ini_set('open_basedir', __DIR__);

// Reusing the cached persistent handle must NOT inherit the pre-tightening
// freedom: check_liveness escalates it by disabling external access.
$b = new PDO('duckdb::memory:', null, null, [PDO::ATTR_PERSISTENT => true]);
$b->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
try {
    $b->query("SELECT * FROM read_csv('/etc/hostname')")->fetchAll();
    echo "BYPASS: read_csv succeeded through the reused persistent handle\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}

// Plain queries still work on the (now sandboxed) handle.
var_dump((int) $b->query('SELECT 42')->fetchColumn());
?>
--EXPECT--
bool(true)
int(42)
