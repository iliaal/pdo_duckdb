--TEST--
pdo_duckdb: Appender builds nested values (LIST/ARRAY/STRUCT/MAP) from PHP arrays
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$db->exec("CREATE TABLE t (
    lst INTEGER[],
    arr INTEGER[3],
    st  STRUCT(x INTEGER, y VARCHAR),
    mp  MAP(VARCHAR, INTEGER),
    nl  INTEGER[][],
    dl  DATE[]
)");

$app = $db->duckdbAppender('t');
$app->appendRow(
    [1, 2, 3],
    [4, 5, 6],
    ['x' => 7, 'y' => 'hi'],
    ['a' => 1, 'b' => 2],
    [[1, 2], [3]],
    ['2026-01-01', '2026-12-31']
);
// empty list + NULL element in a second row
$app->appendRow([], [0, 0, 0], ['x' => 0, 'y' => null], [], [[]], []);
$app->flush();

foreach ($db->query('SELECT * FROM t') as $r) {
    echo $r['lst'], '|', $r['arr'], '|', $r['st'], '|', $r['mp'], '|', $r['nl'], '|', $r['dl'], "\n";
}

// structural errors are reported up front, before anything is appended (so a
// fresh appender on the same table is not poisoned by the rejected row).
function expect_fail(PDO $db, string $table, callable $fn, string $label): void {
    $app = $db->duckdbAppender($table);
    try {
        $fn($app);
        echo "$label: no error (BUG)\n";
    } catch (\Throwable $e) {
        echo "$label: ", get_class($e), "\n";
    }
}
expect_fail($db, 't', fn($a) => $a->appendRow([1], [1, 2], ['x'=>1,'y'=>'z'], [], [], []), 'wrong fixed-array size');
expect_fail($db, 't', fn($a) => $a->appendRow([1], [1,2,3], ['x' => 1], [], [], []), 'missing struct field');

// a PHP array given for a scalar column is a TypeError
$db->exec('CREATE TABLE scalars (n INTEGER)');
expect_fail($db, 'scalars', fn($a) => $a->appendRow([1, 2]), 'array into scalar column');
?>
--EXPECT--
[1, 2, 3]|[4, 5, 6]|{'x': 7, 'y': hi}|{a=1, b=2}|[[1, 2], [3]]|[2026-01-01, 2026-12-31]
[]|[0, 0, 0]|{'x': 0, 'y': NULL}|{}|[[]]|[]
wrong fixed-array size: ValueError
missing struct field: ValueError
array into scalar column: TypeError
--CLEAN--
<?php
?>
