--TEST--
pdo_duckdb: Appender rejects PHP array shapes that do not match nested DuckDB types
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (lst INTEGER[], arr INTEGER[2], st STRUCT(x INTEGER))');
$app = $db->duckdbAppender('t');

foreach ([
    'associative list' => [['left' => 1], [1, 2], ['x' => 3]],
    'associative fixed array' => [[1], ['left' => 2, 'right' => 3], ['x' => 4]],
    'unknown struct field' => [[1], [2, 3], ['x' => 4, 'typo' => 5]],
    'numeric struct field' => [[1], [2, 3], ['x' => 4, 0 => 5]],
] as $label => $row) {
    try {
        $app->appendRow(...$row);
        echo "BAD: $label accepted\n";
    } catch (ValueError $e) {
        echo "$label rejected\n";
    }
}

// Every rejection happens while building the row, before the native appender
// receives a value, so the same appender remains usable.
$app->appendRow([6], [7, 8], ['x' => 9]);
$app->flush();
echo $db->query('SELECT lst::VARCHAR || \'|\' || arr::VARCHAR || \'|\' || st::VARCHAR FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
associative list rejected
associative fixed array rejected
unknown struct field rejected
numeric struct field rejected
[6]|[7, 8]|{'x': 9}
