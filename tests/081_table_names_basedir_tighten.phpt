--TEST--
pdo_duckdb: duckdbTableNames() fails closed when open_basedir is re-narrowed
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Table-name extraction is a SQL entry point: it latches the sandbox, and
// once latched DuckDB allowlists are frozen, so a re-narrowed basedir must
// refuse rather than parse under a stale sandbox.
$base = __DIR__;
$narrow = $base . '/081_narrow_' . getmypid();
@mkdir($narrow, 0700, true);

$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE users (id INTEGER)');

// Latch the sandbox under $base through the tableNames entry point itself.
ini_set('open_basedir', $base);
var_dump($db->duckdbTableNames('SELECT * FROM users'));

// Re-narrow: the recorded basedir no longer matches, so this must fail
// closed instead of returning names.
ini_set('open_basedir', $narrow);
try {
    $db->duckdbTableNames('SELECT * FROM users');
    echo "BAD: tableNames parsed after re-narrow\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'sandbox') ? "fail_closed\n" : ('other=' . $e->getMessage() . "\n");
}
?>
--EXPECT--
array(1) {
  [0]=>
  string(5) "users"
}
fail_closed
--CLEAN--
<?php
foreach (glob(__DIR__ . '/081_narrow_*') ?: [] as $d) {
    @rmdir($d);
}
?>
