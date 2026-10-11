--TEST--
pdo_duckdb: nested FLOAT conversion rejects finite overflow and preserves the appender
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// Adjacent doubles below, at and above the FLOAT round-to-infinity boundary.
// The below-boundary value rounds to FLT_MAX; the other two must be rejected.
$below = 3.4028235677973362e38;
$boundary = 3.4028235677973366e38;
$above = 3.402823567797337e38;
$max = 3.4028234663852886e38;
$cases = [
    ['FLOAT[]', 'c[1]', fn($v) => [$v]],
    ['FLOAT[1]', 'c[1]', fn($v) => [$v]],
    ['FLOAT[][]', 'c[1][1]', fn($v) => [[$v]]],
    ['STRUCT(x FLOAT)', 'c.x', fn($v) => ['x' => $v]],
    ['MAP(VARCHAR, FLOAT)', "map_extract_value(c, 'x')", fn($v) => ['x' => $v]],
];
foreach ($cases as [$type, $projection, $wrap]) {
    $db->exec("CREATE OR REPLACE TABLE t (id INTEGER, c $type)");
    $app = $db->duckdbAppender('t');
    $app->appendRow(0, $wrap(1.5));
    $rejected = 0;
    foreach ([$boundary, $above, 1e100, -$boundary, -$above, -1e100] as $bad) {
        try {
            // A failed second column must leave neither a partial row nor a
            // poisoned appender, and must preserve the earlier buffered row.
            $app->appendRow(1, $wrap($bad));
        } catch (ValueError $e) {
            $rejected++;
        }
    }
    $inputs = [$below, $max, -$below, -$max, 0.0, INF, -INF, NAN];
    $expected = [1.5, $max, $max, -$max, -$max, 0.0, INF, -INF, NAN];
    foreach ($inputs as $i => $value) {
        $app->appendRow($i + 2, $wrap($value));
    }
    $app->close();
    $rows = $db->query("SELECT id, $projection FROM t ORDER BY id")->fetchAll(PDO::FETCH_NUM);
    $ok = count($rows) === count($expected);
    foreach ($rows as $i => [$id, $value]) {
        $want = $expected[$i] ?? null;
        $ok = $ok && $id === ($i === 0 ? 0 : $i + 1)
            && (is_float($want) && is_nan($want) ? is_nan($value) : $value === $want);
    }
    echo "$type: rejected=$rejected recovery=", $ok ? 'ok' : 'BAD', "\n";
}

// DOUBLE leaves must retain large finite values without narrowing.
$db->exec('CREATE TABLE doubles (c DOUBLE[])');
$app = $db->duckdbAppender('doubles');
$app->appendRow([1e100, -1e100])->close();
$row = $db->query('SELECT c[1], c[2] FROM doubles')->fetch(PDO::FETCH_NUM);
echo 'DOUBLE: ', $row === [1e100, -1e100] ? 'ok' : 'BAD', "\n";
?>
--EXPECT--
FLOAT[]: rejected=6 recovery=ok
FLOAT[1]: rejected=6 recovery=ok
FLOAT[][]: rejected=6 recovery=ok
STRUCT(x FLOAT): rejected=6 recovery=ok
MAP(VARCHAR, FLOAT): rejected=6 recovery=ok
DOUBLE: ok
