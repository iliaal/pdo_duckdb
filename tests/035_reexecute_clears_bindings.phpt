--TEST--
pdo_duckdb: re-executing with fewer params clears stale DuckDB bindings
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect() {
    return PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
}

$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$st = $db->prepare('SELECT :a AS a, :b AS b');

// First execute binds both params.
$st->execute([':a' => 'first', ':b' => 'secret']);
$row = $st->fetch(PDO::FETCH_ASSOC);
echo "$row[a],$row[b]\n";

// Re-executing without :b must NOT silently reuse the previous :b value; DuckDB
// should report the now-missing parameter.
try {
    $st->execute([':a' => 'second']);
    echo "BAD: reused stale binding: " . $st->fetchColumn() . "\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'parameters: 2')
        || str_contains($e->getMessage(), 'not provided')
        || str_contains($e->getMessage(), 'Values were not'));
}

// Binding both again still works (latch re-armed for each execute).
$st->execute([':a' => 'x', ':b' => 'y']);
$row = $st->fetch(PDO::FETCH_ASSOC);
echo "$row[a],$row[b]\n";
?>
--EXPECT--
first,secret
bool(true)
x,y
