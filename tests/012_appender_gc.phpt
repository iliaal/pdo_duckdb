--TEST--
pdo_duckdb: Appender is cycle-collectable (get_gc), not serializable, strict props
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect() {
    return PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
}

// get_gc: appender holds a ref to the PDO object; form a cycle and confirm the
// collector reclaims it (without get_gc the appender + connection would leak).
$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (i INTEGER)');
$a = $db->duckdbAppender('t');
@($db->ref = $a);                 // PDO -> appender (appender -> PDO is internal)
$w = WeakReference::create($a);
unset($a, $db);
gc_collect_cycles();
var_dump($w->get() === null);

// not serializable
$db2 = connect();
$db2->exec('CREATE TABLE t (i INTEGER)');
$app = $db2->duckdbAppender('t');
try {
    serialize($app);
    echo "BAD: serialized\n";
} catch (\Exception $e) {
    var_dump(str_contains($e->getMessage(), 'not allowed'));
}

// strict properties (no dynamic props)
try {
    $app->nope = 1;
    echo "BAD: dynamic property allowed\n";
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'dynamic property'));
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
