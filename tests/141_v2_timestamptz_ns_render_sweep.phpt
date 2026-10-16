--TEST--
pdo_duckdb: DuckDB 2.0 TIMESTAMPTZ_NS rendering matches the engine across a seeded value sweep
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("SET TimeZone='UTC'");

// Edges, then a seeded sweep: the full int64 nanosecond range, 1840..2100 at
// nanosecond, whole-second and whole-microsecond precision, and instants just
// under a whole second before the epoch.
$ns = [PHP_INT_MIN, PHP_INT_MIN + 2, PHP_INT_MAX - 1, -1, 0, 1, 999, 1000, 1001,
    -500, -999, -1000, -1001, 86399999999999, -86400000000000, -86400000000001];
mt_srand(20261016);
for ($i = 0; count($ns) < 800; $i++) {
    switch ($i % 5) {
        case 0: $ns[] = mt_rand(PHP_INT_MIN, PHP_INT_MAX); break;
        case 1: $ns[] = mt_rand(-4102444800, 4102444800) * 1000000000 + mt_rand(0, 999999999); break;
        case 2: $ns[] = mt_rand(-4102444800, 4102444800) * 1000000000; break;
        case 3: $ns[] = mt_rand(-4102444800000000, 4102444800000000) * 1000; break;
        case 4: $ns[] = -mt_rand(0, 4102444800) * 1000000000 - mt_rand(1, 999); break;
    }
}

// DuckDB's own CAST(TIMESTAMPTZ_NS AS VARCHAR) misrenders, or raises an
// INTERNAL error for, a pre-epoch instant less than 1 us before a whole
// second. Those rows are checked against the TIMESTAMP_NS rendering only.
$db->exec('CREATE TABLE f (id INTEGER, x TIMESTAMPTZ_NS, cast_ok BOOLEAN)');
$ins = $db->prepare('INSERT INTO f SELECT ?, make_timestamp_ns(?::BIGINT)::TIMESTAMPTZ_NS, ?::INTEGER = 1');
foreach ($ns as $id => $n) {
    $ins->execute([$id, $n, ($n < 0 && $n % 1000 !== 0 && intdiv($n, 1000) % 1000000 === 0) ? 0 : 1]);
}
$db->exec("INSERT INTO f VALUES (-1, 'infinity', true), (-2, '-infinity', true)");

$shape = "x, [x] AS l, {'t': x} AS s, MAP {'k': x} AS m, [x, x]::TIMESTAMPTZ_NS[2] AS a, union_value(t := x) AS u";
$got = $db->query("SELECT id, $shape, x::TIMESTAMP_NS::VARCHAR AS ns FROM f ORDER BY id")->fetchAll(PDO::FETCH_ASSOC | PDO::FETCH_UNIQUE);
$cast = "CAST(x AS VARCHAR) AS x, CAST([x] AS VARCHAR) AS l, CAST({'t': x} AS VARCHAR) AS s,"
    . " CAST(MAP {'k': x} AS VARCHAR) AS m, CAST([x, x]::TIMESTAMPTZ_NS[2] AS VARCHAR) AS a,"
    . " CAST(union_value(t := x) AS VARCHAR) AS u";
$want = $db->query("SELECT id, $cast FROM f WHERE cast_ok ORDER BY id")->fetchAll(PDO::FETCH_ASSOC | PDO::FETCH_UNIQUE);

$ny = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$ny->exec("SET TimeZone='America/New_York'");
$ny->exec('CREATE TABLE f (id INTEGER, x TIMESTAMPTZ_NS)');
$copy = $ny->prepare('INSERT INTO f VALUES (?, ?::TIMESTAMPTZ_NS)');
foreach ($got as $id => $row) {
    $copy->execute([$id, $row['x']]);
}
$ny_got = $ny->query("SELECT id, $shape FROM f ORDER BY id")->fetchAll(PDO::FETCH_ASSOC | PDO::FETCH_UNIQUE);

$template = $want[0];
$cases = $scalar_bad = $nested_bad = $tz_bad = $engine_checked = 0;
foreach ($got as $id => $row) {
    $cases++;
    $plain = $row['ns'];
    unset($row['ns']);
    $oracle = preg_match('/^\d/', $plain) ? "$plain+00" : $plain;
    if ($row['x'] !== $oracle) {
        $scalar_bad++;
        echo "SCALAR id=$id got={$row['x']} want=$oracle\n";
    }
    if (isset($want[$id])) {
        $engine_checked++;
        $expect = $want[$id];
    } else {
        $expect = [];
        foreach ($template as $k => $v) {
            $expect[$k] = str_replace($template['x'], $oracle, $v);
        }
    }
    foreach ($expect as $k => $v) {
        if ($row[$k] !== $v) {
            $k === 'x' ? $scalar_bad++ : $nested_bad++;
            echo "MISMATCH id=$id $k got={$row[$k]} want=$v\n";
        }
    }
    if ($ny_got[$id] !== $row) {
        $tz_bad++;
        echo "TIMEZONE id=$id\n";
    }
}
echo "cases=$cases engine_checked=$engine_checked scalar_bad=$scalar_bad nested_bad=$nested_bad tz_bad=$tz_bad\n";
?>
--EXPECT--
cases=802 engine_checked=642 scalar_bad=0 nested_bad=0 tz_bad=0
