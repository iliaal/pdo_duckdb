--TEST--
pdo_duckdb: a persistent handle with quack loaded is replaced, not reused, under a tightened open_basedir
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
try {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('INSTALL httpfs; INSTALL quack; LOAD httpfs; LOAD quack');
} catch (PDOException $e) {
    die('skip quack extension not available: ' . strtok($e->getMessage(), "\n"));
}
?>
--FILE--
<?php
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];

// exec() only: a query() result would keep the PDO object (and the persistent
// handle's refcount) alive past unset().
$a = new PDO('duckdb::memory:', null, null, $opts);
if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
    $a->exec('SET extension_directory=' . $a->quote($dir));
}
$a->exec('LOAD httpfs; LOAD quack');
$a->exec('CREATE TABLE marker AS SELECT 1 AS v');
unset($a);

ini_set('open_basedir', __DIR__);

// The checkout must not hand back the instance with quack loaded; it gets a
// fresh one, sandboxed at open.
$b = new PDO('duckdb::memory:', null, null, $opts);
try {
    $b->query('SELECT v FROM marker')->fetchAll();
    echo "marker: REUSED\n";
} catch (PDOException $e) {
    echo "marker: ", str_contains($e->getMessage(), 'marker') ? 'absent' : strtok($e->getMessage(), "\n"), "\n";
}
var_dump((int) $b->query("SELECT count(*) FROM duckdb_functions() WHERE function_name IN ('quack_serve', 'quack_query')")->fetchColumn());
try {
    $b->exec('LOAD quack');
    echo "LOAD quack: ALLOWED\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled through configuration'));
}
var_dump((int) $b->query('SELECT 42')->fetchColumn());
?>
--EXPECT--
marker: absent
int(0)
bool(true)
int(42)
