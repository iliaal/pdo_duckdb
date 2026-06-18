--TEST--
pdo_duckdb: ATTR_AUTOCOMMIT accept-on / reject-off semantics
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect($dsn = 'duckdb::memory:') {
    return PHP_VERSION_ID >= 80400 ? PDO::connect($dsn) : new PDO($dsn);
}

$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// Autocommit on is DuckDB's real behaviour -> accepted.
var_dump($db->setAttribute(PDO::ATTR_AUTOCOMMIT, true));

// Turning autocommit off is unsupported -> rejected, not silently swallowed.
try {
    $db->setAttribute(PDO::ATTR_AUTOCOMMIT, false);
    echo "BAD: autocommit=false accepted\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'does not support disabling autocommit'));
}

// Explicit transactions still work regardless.
$db->exec('CREATE TABLE t (i INTEGER)');
$db->beginTransaction();
$db->exec('INSERT INTO t VALUES (1), (2)');
$db->commit();
var_dump((int) $db->query('SELECT count(*) FROM t')->fetchColumn());

// autocommit=true as a constructor option must not raise (no IM001).
try {
    new PDO('duckdb::memory:', null, null, [PDO::ATTR_AUTOCOMMIT => true]);
    echo "ctor autocommit=true: ok\n";
} catch (\Throwable $e) {
    echo "BAD: ctor autocommit=true raised: {$e->getMessage()}\n";
}
?>
--EXPECT--
bool(true)
bool(true)
int(2)
ctor autocommit=true: ok
